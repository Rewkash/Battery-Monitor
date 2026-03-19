#include "platform/windows/ZmiVendorBatteryHints.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>
#include <cfgmgr32.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include "platform/windows/BluetoothPnpUtils.h"
#include "platform/windows/WindowsBluetoothConstants.h"
#include "platform/windows/ZmiBatteryCodec.h"

namespace battery_monitor {

namespace {

using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::Foundation::IPropertyValue;
using winrt::Windows::Foundation::PropertyType;

void LogDebug(bool debug_enabled, XiaomiDebugLogFn debug_log, const std::string& message) {
    if (debug_enabled && debug_log != nullptr) {
        debug_log(message);
    }
}

std::string ToUtf8(const winrt::hstring& value) {
    return winrt::to_string(value);
}

std::string ToUtf8(const std::wstring& value) {
    return winrt::to_string(winrt::hstring(value));
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string BytesToHex(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return {};
    }

    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(bytes.size() * 3U);
    for (const auto value : bytes) {
        output.push_back(kHex[(value >> 4U) & 0x0FU]);
        output.push_back(kHex[value & 0x0FU]);
        output.push_back(' ');
    }
    return output;
}

bool TryGetPropertyValue(const DeviceInformation& device_info, const wchar_t* property_name, IInspectable* value) {
    if (value == nullptr) {
        return false;
    }

    const auto properties = device_info.Properties();
    const winrt::hstring key(property_name);
    if (!properties.HasKey(key)) {
        return false;
    }

    *value = properties.Lookup(key);
    return *value != nullptr;
}

std::vector<BatteryReading> BuildReadingsFromTriplet(const XiaomiBatterySnapshot& snapshot) {
    std::vector<BatteryReading> readings;
    if (snapshot.left.has_value()) {
        readings.push_back(BatteryReading{"left", *snapshot.left});
    }
    if (snapshot.right.has_value()) {
        readings.push_back(BatteryReading{"right", *snapshot.right});
    }
    if (snapshot.case_level.has_value()) {
        readings.push_back(BatteryReading{"case", *snapshot.case_level});
    }
    return readings;
}

int ZmiSnapshotPresenceCount(const XiaomiBatterySnapshot& snapshot) {
    int count = 0;
    if (snapshot.left.has_value()) {
        ++count;
    }
    if (snapshot.right.has_value()) {
        ++count;
    }
    if (snapshot.case_level.has_value()) {
        ++count;
    }
    return count;
}

std::vector<int> ExtractIntegersFromText(const std::string& text) {
    std::vector<int> numbers;
    int current = -1;
    for (const char ch : text) {
        if (ch >= '0' && ch <= '9') {
            if (current < 0) {
                current = 0;
            }
            current = (current * 10) + static_cast<int>(ch - '0');
            continue;
        }
        if (current >= 0) {
            numbers.push_back(current);
            current = -1;
        }
    }
    if (current >= 0) {
        numbers.push_back(current);
    }
    return numbers;
}

std::vector<BatteryReading> DecodeZmiVendorBatteryReadingsFromUInt32(std::uint32_t raw_value);

std::vector<BatteryReading> DecodeZmiVendorReadingsFromNumericValues(const std::vector<int>& values) {
    struct Candidate {
        XiaomiBatterySnapshot snapshot;
        int score = -1;
        int presence = 0;
    };

    if (values.empty()) {
        return {};
    }

    auto evaluate_triplet = [](int left_raw, int right_raw, int case_raw, int pattern_bonus) -> Candidate {
        XiaomiBatterySnapshot snapshot;
        snapshot.left = NormalizeZmiVendorBatteryScalar(left_raw);
        snapshot.right = NormalizeZmiVendorBatteryScalar(right_raw);
        snapshot.case_level = NormalizeZmiVendorBatteryScalar(case_raw);

        const int presence = ZmiSnapshotPresenceCount(snapshot);
        if (presence < 2) {
            return Candidate{snapshot, -1, presence};
        }

        int score = pattern_bonus;
        score += presence * 16;
        if (presence == 3) {
            score += 8;
        }
        if (snapshot.left.has_value() && snapshot.right.has_value() && *snapshot.left != *snapshot.right) {
            score += 6;
        }
        if ((snapshot.left.has_value() && *snapshot.left > 10U) ||
            (snapshot.right.has_value() && *snapshot.right > 10U) ||
            (snapshot.case_level.has_value() && *snapshot.case_level > 10U)) {
            score += 5;
        }
        if (snapshot.left.has_value() && snapshot.right.has_value() && snapshot.case_level.has_value() &&
            *snapshot.left == *snapshot.right && *snapshot.right == *snapshot.case_level) {
            score -= 7;
        }

        return Candidate{snapshot, score, presence};
    };

    std::optional<Candidate> best;
    auto consider = [&](int left_raw, int right_raw, int case_raw, int pattern_bonus) {
        const auto candidate = evaluate_triplet(left_raw, right_raw, case_raw, pattern_bonus);
        if (candidate.score < 0) {
            return;
        }
        if (!best.has_value() || candidate.score > best->score) {
            best = candidate;
        }
    };

    for (std::size_t index = 0; index + 2U < values.size(); ++index) {
        int pattern_bonus = 0;
        if (index >= 4U && values[index - 4U] == 0x02 && values[index - 3U] == 0x02 &&
            values[index - 2U] == 0x04 && values[index - 1U] == 0x07) {
            pattern_bonus += 30;
        }
        if (index >= 2U && values[index - 2U] >= 4 && values[index - 2U] <= 8 &&
            values[index - 1U] >= 0 && values[index - 1U] <= 0x7F) {
            pattern_bonus += 18;
        }

        consider(values[index], values[index + 1U], values[index + 2U], pattern_bonus);
    }

    if (best.has_value()) {
        const int threshold = best->presence == 3 ? 58 : 50;
        if (best->score >= threshold) {
            return BuildReadingsFromTriplet(best->snapshot);
        }
    }

    for (const auto raw : values) {
        const auto scalar = NormalizeZmiVendorBatteryScalar(raw);
        if (scalar.has_value()) {
            return {BatteryReading{"main", *scalar}};
        }
    }

    return {};
}

std::vector<BatteryReading> DecodeZmiVendorReadingsFromByteBlob(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return {};
    }

    std::vector<int> values;
    values.reserve(bytes.size());
    for (const auto value : bytes) {
        values.push_back(static_cast<int>(value));
    }

    auto decoded = DecodeZmiVendorReadingsFromNumericValues(values);
    if (decoded.size() >= 2U) {
        return decoded;
    }

    if (bytes.size() >= sizeof(std::uint32_t)) {
        std::vector<BatteryReading> best_packed = decoded;
        for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= bytes.size(); ++offset) {
            std::uint32_t packed = 0;
            std::memcpy(&packed, bytes.data() + static_cast<std::ptrdiff_t>(offset), sizeof(packed));
            auto packed_decoded = DecodeZmiVendorBatteryReadingsFromUInt32(packed);
            if (packed_decoded.size() > best_packed.size()) {
                best_packed = std::move(packed_decoded);
            }
        }
        if (!best_packed.empty()) {
            return best_packed;
        }
    }

    return decoded;
}

std::vector<BatteryReading> DecodeZmiVendorBatteryReadingsFromUInt32(std::uint32_t raw_value) {
    struct Candidate {
        XiaomiBatterySnapshot snapshot;
        int score = -1;
        int presence = 0;
    };

    auto evaluate_triplet = [](std::uint8_t left_raw, std::uint8_t right_raw, std::uint8_t case_raw) -> Candidate {
        XiaomiBatterySnapshot snapshot;
        snapshot.left = NormalizeZmiVendorBatteryScalar(left_raw);
        snapshot.right = NormalizeZmiVendorBatteryScalar(right_raw);
        snapshot.case_level = NormalizeZmiVendorBatteryScalar(case_raw);

        const int presence = ZmiSnapshotPresenceCount(snapshot);
        int score = 0;
        score += presence * 20;
        if (presence == 3) {
            score += 30;
        }
        if (snapshot.left.has_value() && snapshot.right.has_value() && *snapshot.left != *snapshot.right) {
            score += 8;
        }
        if ((snapshot.left.has_value() && *snapshot.left == 100U) ||
            (snapshot.right.has_value() && *snapshot.right == 100U) ||
            (snapshot.case_level.has_value() && *snapshot.case_level == 100U)) {
            score += 4;
        }

        return Candidate{snapshot, score, presence};
    };

    const std::array<std::uint8_t, 4> little_endian_bytes = {
        static_cast<std::uint8_t>((raw_value >> 0U) & 0xFFU),
        static_cast<std::uint8_t>((raw_value >> 8U) & 0xFFU),
        static_cast<std::uint8_t>((raw_value >> 16U) & 0xFFU),
        static_cast<std::uint8_t>((raw_value >> 24U) & 0xFFU),
    };

    std::optional<Candidate> best;
    auto consider = [&](std::uint8_t left_raw, std::uint8_t right_raw, std::uint8_t case_raw) {
        const auto candidate = evaluate_triplet(left_raw, right_raw, case_raw);
        if (candidate.presence < 2) {
            return;
        }
        if (!best.has_value() || candidate.score > best->score) {
            best = candidate;
        }
    };

    consider(little_endian_bytes[0], little_endian_bytes[1], little_endian_bytes[2]);
    consider(little_endian_bytes[1], little_endian_bytes[2], little_endian_bytes[3]);
    consider(little_endian_bytes[2], little_endian_bytes[1], little_endian_bytes[0]);
    consider(little_endian_bytes[3], little_endian_bytes[2], little_endian_bytes[1]);

    if (best.has_value()) {
        const int threshold = best->presence == 3 ? 70 : 55;
        if (best->score >= threshold) {
            return BuildReadingsFromTriplet(best->snapshot);
        }
    }

    const auto scalar = NormalizeZmiVendorBatteryScalar(static_cast<int>(raw_value));
    if (scalar.has_value()) {
        return {BatteryReading{"main", *scalar}};
    }

    return {};
}

std::vector<BatteryReading> DecodeFromInspectable(const IInspectable& value) {
    if (value == nullptr) {
        return {};
    }

    try {
        const auto normalized = NormalizeZmiVendorBatteryScalar(static_cast<int>(winrt::unbox_value<std::uint8_t>(value)));
        if (normalized.has_value()) {
            return {BatteryReading{"main", *normalized}};
        }
    } catch (const winrt::hresult_error&) {
    }

    try {
        const auto uint32_value = winrt::unbox_value<std::uint32_t>(value);
        auto decoded = DecodeZmiVendorBatteryReadingsFromUInt32(uint32_value);
        if (!decoded.empty()) {
            return decoded;
        }
    } catch (const winrt::hresult_error&) {
    }

    try {
        const auto int32_value = winrt::unbox_value<std::int32_t>(value);
        if (int32_value > 0) {
            auto decoded = DecodeZmiVendorBatteryReadingsFromUInt32(static_cast<std::uint32_t>(int32_value));
            if (!decoded.empty()) {
                return decoded;
            }
            const auto normalized = NormalizeZmiVendorBatteryScalar(int32_value);
            if (normalized.has_value()) {
                return {BatteryReading{"main", *normalized}};
            }
        }
    } catch (const winrt::hresult_error&) {
    }

    if (const auto property_value = value.try_as<IPropertyValue>()) {
        try {
            const auto type = property_value.Type();
            if (type == PropertyType::UInt8Array) {
                winrt::com_array<std::uint8_t> values;
                property_value.GetUInt8Array(values);
                std::vector<std::uint8_t> bytes(values.begin(), values.end());
                auto decoded = DecodeZmiVendorReadingsFromByteBlob(bytes);
                if (!decoded.empty()) {
                    return decoded;
                }
            } else if (type == PropertyType::UInt16Array) {
                winrt::com_array<std::uint16_t> values;
                property_value.GetUInt16Array(values);
                std::vector<int> numbers;
                numbers.reserve(values.size());
                for (const auto raw : values) {
                    numbers.push_back(static_cast<int>(raw));
                }
                auto decoded = DecodeZmiVendorReadingsFromNumericValues(numbers);
                if (!decoded.empty()) {
                    return decoded;
                }
            } else if (type == PropertyType::UInt32Array) {
                winrt::com_array<std::uint32_t> values;
                property_value.GetUInt32Array(values);
                std::vector<BatteryReading> best_decoded;
                std::vector<int> numbers;
                numbers.reserve(values.size());
                for (const auto raw : values) {
                    auto decoded = DecodeZmiVendorBatteryReadingsFromUInt32(raw);
                    if (decoded.size() > best_decoded.size()) {
                        best_decoded = std::move(decoded);
                    }
                    if (raw <= static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
                        numbers.push_back(static_cast<int>(raw));
                    }
                }
                if (!best_decoded.empty()) {
                    return best_decoded;
                }
                auto decoded = DecodeZmiVendorReadingsFromNumericValues(numbers);
                if (!decoded.empty()) {
                    return decoded;
                }
            } else if (type == PropertyType::Int32Array) {
                winrt::com_array<std::int32_t> values;
                property_value.GetInt32Array(values);
                std::vector<int> numbers;
                numbers.reserve(values.size());
                for (const auto raw : values) {
                    numbers.push_back(raw);
                }
                auto decoded = DecodeZmiVendorReadingsFromNumericValues(numbers);
                if (!decoded.empty()) {
                    return decoded;
                }
            } else if (type == PropertyType::StringArray) {
                winrt::com_array<winrt::hstring> values;
                property_value.GetStringArray(values);
                for (const auto& text : values) {
                    auto decoded = DecodeZmiVendorReadingsFromNumericValues(ExtractIntegersFromText(ToUtf8(text)));
                    if (!decoded.empty()) {
                        return decoded;
                    }
                }
            }
        } catch (const winrt::hresult_error&) {
        }
    }

    try {
        const auto text = ToUtf8(winrt::unbox_value<winrt::hstring>(value));
        auto decoded = DecodeZmiVendorReadingsFromNumericValues(ExtractIntegersFromText(text));
        if (!decoded.empty()) {
            return decoded;
        }
    } catch (const winrt::hresult_error&) {
    }

    return {};
}

}  // namespace

void AppendZmiVendorHintPropertyRequests(
    const winrt::Windows::Foundation::Collections::IVector<winrt::hstring>& requested_properties) {
    constexpr int kPropertyIdMin = 1;
    constexpr int kPropertyIdMax = 32;
    for (int property_id = kPropertyIdMin; property_id <= kPropertyIdMax; ++property_id) {
        requested_properties.Append(winrt::to_hstring(
            "{670245F9-6E25-4179-85C1-981C33B9D3B7} " + std::to_string(property_id)));
    }
}

std::vector<BatteryReading> ReadZmiVendorBatteryHintFromPnpAddress(std::uint64_t address,
                                                                   bool debug_enabled,
                                                                   XiaomiDebugLogFn debug_log) {
    static std::unordered_map<std::uint64_t, std::vector<BatteryReading>> cache;
    if (const auto cached = cache.find(address); cached != cache.end()) {
        return cached->second;
    }

    const auto instance_ids = FindBthEnumInstanceIdsByAddress(address);
    if (instance_ids.empty()) {
        LogDebug(debug_enabled, debug_log,
                 "ZMI PnP lookup: instance id not found for address=" + std::to_string(address));
        cache[address] = {};
        return {};
    }

    std::wstringstream prefix_builder;
    prefix_builder << L"BTHENUM\\DEV_" << std::uppercase << std::hex
                   << std::setw(12) << std::setfill(L'0') << address;
    const std::wstring preferred_prefix = prefix_builder.str();

    std::vector<std::wstring> ranked_instance_ids = instance_ids;
    std::stable_sort(ranked_instance_ids.begin(), ranked_instance_ids.end(),
                     [&preferred_prefix](const std::wstring& lhs, const std::wstring& rhs) {
                         auto score = [&preferred_prefix](const std::wstring& value) {
                             int rank = 0;
                             if (value.rfind(preferred_prefix, 0) == 0) {
                                 rank += 100;
                             }
                             if (value.find(L"\\DEV_") != std::wstring::npos) {
                                 rank += 20;
                             }
                             if (value.find(L"{") != std::wstring::npos) {
                                 rank -= 12;
                             }
                             if (value.find(L"LOCALMFG") != std::wstring::npos) {
                                 rank -= 8;
                             }
                             return rank;
                         };
                         return score(lhs) > score(rhs);
                     });

    for (const auto& instance_id : ranked_instance_ids) {
        DEVINST dev_inst = 0;
        const auto locate_result =
            CM_Locate_DevNodeW(&dev_inst, const_cast<wchar_t*>(instance_id.c_str()), CM_LOCATE_DEVNODE_NORMAL);
        if (locate_result != CR_SUCCESS) {
            continue;
        }

        auto decode_property = [&](const DEVPROPKEY& property_key) -> std::vector<BatteryReading> {
            DEVPROPTYPE property_type = DEVPROP_TYPE_EMPTY;
            const auto raw_data = ReadDevNodePropertyRaw(dev_inst, property_key, &property_type);
            if (!raw_data.has_value()) {
                return {};
            }

            if (debug_enabled && (property_key.pid != 4U || raw_data->size() > 1U)) {
                const std::size_t preview_len = std::min<std::size_t>(raw_data->size(), 24U);
                std::vector<std::uint8_t> preview(raw_data->begin(),
                                                  raw_data->begin() + static_cast<std::ptrdiff_t>(preview_len));
                LogDebug(debug_enabled, debug_log,
                         "ZMI PnP raw pid=" + std::to_string(property_key.pid) +
                             " type=" + std::to_string(property_type) +
                             " size=" + std::to_string(raw_data->size()) +
                             " data=" + BytesToHex(preview));
            }

            if (property_type == DEVPROP_TYPE_BYTE && !raw_data->empty()) {
                const auto normalized = NormalizeZmiVendorBatteryScalar(static_cast<int>((*raw_data)[0]));
                if (normalized.has_value()) {
                    return {BatteryReading{"main", *normalized}};
                }
            }
            if (property_type == DEVPROP_TYPE_UINT32 && raw_data->size() >= sizeof(std::uint32_t)) {
                std::uint32_t value = 0;
                std::memcpy(&value, raw_data->data(), sizeof(value));
                auto decoded = DecodeZmiVendorBatteryReadingsFromUInt32(value);
                if (!decoded.empty()) {
                    return decoded;
                }
            }
            if (property_type == DEVPROP_TYPE_INT32 && raw_data->size() >= sizeof(std::int32_t)) {
                std::int32_t value = 0;
                std::memcpy(&value, raw_data->data(), sizeof(value));
                if (value > 0) {
                    auto decoded = DecodeZmiVendorBatteryReadingsFromUInt32(static_cast<std::uint32_t>(value));
                    if (!decoded.empty()) {
                        return decoded;
                    }
                }
            }

            return DecodeZmiVendorReadingsFromByteBlob(*raw_data);
        };

        std::vector<DEVPROPKEY> keys_to_probe;
        constexpr int kVendorPidMin = 1;
        constexpr int kVendorPidMax = 32;
        for (int pid = kVendorPidMin; pid <= kVendorPidMax; ++pid) {
            DEVPROPKEY key = kZmiVendorBatteryHintPropKey;
            key.pid = static_cast<ULONG>(pid);
            keys_to_probe.push_back(key);
        }

        ULONG key_count = 0;
        const auto key_count_result = CM_Get_DevNode_Property_Keys(dev_inst, nullptr, &key_count, 0);
        if ((key_count_result == CR_SUCCESS || key_count_result == CR_BUFFER_SMALL) && key_count > 0U) {
            std::vector<DEVPROPKEY> property_keys(key_count);
            if (CM_Get_DevNode_Property_Keys(dev_inst, property_keys.data(), &key_count, 0) == CR_SUCCESS) {
                property_keys.resize(key_count);
                for (const auto& property_key : property_keys) {
                    if (std::memcmp(&property_key.fmtid, &kZmiVendorBatteryHintPropKey.fmtid, sizeof(GUID)) != 0) {
                        continue;
                    }
                    const bool already_added = std::any_of(
                        keys_to_probe.begin(), keys_to_probe.end(), [&property_key](const DEVPROPKEY& existing) {
                            return std::memcmp(&existing, &property_key, sizeof(DEVPROPKEY)) == 0;
                        });
                    if (!already_added) {
                        keys_to_probe.push_back(property_key);
                    }
                }
            }
        }

        std::sort(keys_to_probe.begin(), keys_to_probe.end(),
                  [](const DEVPROPKEY& lhs, const DEVPROPKEY& rhs) { return lhs.pid < rhs.pid; });
        keys_to_probe.erase(
            std::unique(keys_to_probe.begin(), keys_to_probe.end(),
                        [](const DEVPROPKEY& lhs, const DEVPROPKEY& rhs) {
                            return std::memcmp(&lhs, &rhs, sizeof(DEVPROPKEY)) == 0;
                        }),
            keys_to_probe.end());

        if (debug_enabled && keys_to_probe.size() > 1U) {
            std::string pid_list;
            for (const auto& property_key : keys_to_probe) {
                if (!pid_list.empty()) {
                    pid_list += ",";
                }
                pid_list += std::to_string(property_key.pid);
            }
            LogDebug(debug_enabled, debug_log,
                     "ZMI PnP lookup: probing vendor pids=" + pid_list +
                         " instance='" + ToUtf8(instance_id) + "'");
        }

        std::vector<BatteryReading> best_single_value;
        for (const auto& property_key : keys_to_probe) {
            auto decoded = decode_property(property_key);
            if (decoded.empty()) {
                continue;
            }

            if (decoded.size() >= 2U) {
                LogDebug(debug_enabled, debug_log,
                         "ZMI PnP lookup: vendor property pid=" + std::to_string(property_key.pid) +
                             " decoded entries=" + std::to_string(decoded.size()));
                cache[address] = decoded;
                return decoded;
            }

            if (debug_enabled) {
                LogDebug(debug_enabled, debug_log,
                         "ZMI PnP lookup: vendor property pid=" + std::to_string(property_key.pid) +
                             " decoded scalar entries=" + std::to_string(decoded.size()));
            }
            if (best_single_value.empty()) {
                best_single_value = decoded;
            }
        }

        if (!best_single_value.empty()) {
            cache[address] = best_single_value;
            return best_single_value;
        }
    }

    LogDebug(debug_enabled, debug_log,
             "ZMI PnP lookup: vendor property missing for address=" + std::to_string(address));
    cache[address] = {};
    return {};
}

std::vector<BatteryReading> ReadZmiVendorBatteryHint(const DeviceInformation& endpoint_info,
                                                     bool debug_enabled,
                                                     XiaomiDebugLogFn debug_log) {
    constexpr auto kVendorBatteryHintKey = L"{670245F9-6E25-4179-85C1-981C33B9D3B7} 4";
    constexpr auto kVendorGuidPrefixLower = "{670245f9-6e25-4179-85c1-981c33b9d3b7}";

    std::vector<BatteryReading> best_single_value;
    std::unordered_set<std::string> processed_keys;

    IInspectable raw_value = nullptr;
    if (!TryGetPropertyValue(endpoint_info, kVendorBatteryHintKey, &raw_value)) {
        LogDebug(debug_enabled, debug_log, "ZMI vendor key #4 was not present in DeviceInformation properties");
    } else {
        auto decoded = DecodeFromInspectable(raw_value);
        if (decoded.size() >= 2U) {
            LogDebug(debug_enabled, debug_log,
                     "ZMI vendor key #4 decoded entries=" + std::to_string(decoded.size()));
            return decoded;
        }
        if (!decoded.empty()) {
            best_single_value = decoded;
        }
        processed_keys.insert(ToLowerAscii(ToUtf8(winrt::to_hstring(kVendorBatteryHintKey))));
    }

    for (const auto& property : endpoint_info.Properties()) {
        const std::string key = ToLowerAscii(ToUtf8(property.Key()));
        if (key.find(kVendorGuidPrefixLower) == std::string::npos) {
            continue;
        }
        if (!processed_keys.insert(key).second) {
            continue;
        }

        auto decoded = DecodeFromInspectable(property.Value());
        if (decoded.size() >= 2U) {
            LogDebug(debug_enabled, debug_log,
                     "ZMI vendor key '" + key + "' decoded entries=" + std::to_string(decoded.size()));
            return decoded;
        }
        if (debug_enabled && !decoded.empty()) {
            LogDebug(debug_enabled, debug_log,
                     "ZMI vendor key '" + key + "' decoded scalar entries=" + std::to_string(decoded.size()));
        }
        if (!decoded.empty() && best_single_value.empty()) {
            best_single_value = decoded;
        }
    }

    if (!best_single_value.empty()) {
        return best_single_value;
    }

    LogDebug(debug_enabled, debug_log, "ZMI vendor key type was not supported");
    return {};
}

}  // namespace battery_monitor
