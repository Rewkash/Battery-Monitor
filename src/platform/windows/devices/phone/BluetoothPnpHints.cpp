#include "platform/windows/devices/phone/BluetoothPnpHints.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <windows.h>
#include <cfgmgr32.h>
#include <winrt/base.h>

#include "platform/windows/bluetooth/BluetoothPnpUtils.h"
#include "platform/windows/shared/WindowsBluetoothConstants.h"

namespace battery_monitor {

namespace {

void LogDebug(bool debug_enabled, XiaomiDebugLogFn debug_log, const std::string& message) {
    if (debug_enabled && debug_log != nullptr) {
        debug_log(message);
    }
}

std::string ToUtf8(const std::wstring& value) {
    return winrt::to_string(winrt::hstring(value));
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

void AppendUniqueStrings(std::vector<std::string>* target, const std::vector<std::string>& incoming) {
    if (target == nullptr) {
        return;
    }

    for (const auto& value : incoming) {
        if (value.empty()) {
            continue;
        }
        if (std::find(target->begin(), target->end(), value) == target->end()) {
            target->push_back(value);
        }
    }
}

std::optional<std::uint8_t> NormalizePhoneBatteryHintScalar(int raw_value) {
    if (raw_value < 0 || raw_value == 255) {
        return std::nullopt;
    }
    if (raw_value <= 100) {
        return static_cast<std::uint8_t>(raw_value);
    }
    if ((raw_value & 0xFF) == raw_value) {
        const int masked = raw_value & 0x7F;
        if (masked >= 0 && masked <= 100) {
            return static_cast<std::uint8_t>(masked);
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<PnpBluetoothVisualHints> ReadBluetoothVisualHintsFromPnpAddress(std::uint64_t address) {
    if (address <= 0xFFFFULL) {
        return std::nullopt;
    }

    const auto instance_ids = FindBthEnumInstanceIdsByAddress(address);
    std::vector<std::wstring> ranked_instance_ids = instance_ids;
    const auto bthle_instance_ids = FindBthLeInstanceIdsByAddress(address);
    ranked_instance_ids.insert(ranked_instance_ids.end(), bthle_instance_ids.begin(), bthle_instance_ids.end());
    if (ranked_instance_ids.empty()) {
        return std::nullopt;
    }

    std::wstringstream prefix_builder;
    prefix_builder << L"BTHENUM\\DEV_" << std::uppercase << std::hex
                   << std::setw(12) << std::setfill(L'0') << address;
    const std::wstring preferred_prefix = prefix_builder.str();
    std::stable_sort(ranked_instance_ids.begin(), ranked_instance_ids.end(),
                     [&preferred_prefix](const std::wstring& lhs, const std::wstring& rhs) {
                         auto score = [&preferred_prefix](const std::wstring& value) {
                             int rank = 0;
                             if (value.rfind(preferred_prefix, 0) == 0) {
                                 rank += 100;
                             }
                             if (value.rfind(L"BTHLE\\DEV_", 0) == 0) {
                                 rank += 90;
                             }
                             if (value.find(L"\\DEV_") != std::wstring::npos) {
                                 rank += 20;
                             }
                             if (value.find(L"{") != std::wstring::npos) {
                                 rank -= 12;
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

        PnpBluetoothVisualHints hints;
        if (const auto class_of_device = ReadDevNodeUInt32Property(dev_inst, kBluetoothClassOfDevicePropKey);
            class_of_device.has_value()) {
            hints.bluetooth_cod_major = ((*class_of_device >> 8U) & 0x1FU);
            hints.bluetooth_cod_minor = ((*class_of_device >> 2U) & 0x3FU);
        }
        AppendUniqueStrings(&hints.device_categories, ReadDevNodeStringListProperty(dev_inst, kDeviceContainerCategoryPropKey));
        AppendUniqueStrings(
            &hints.device_categories, ReadDevNodeStringListProperty(dev_inst, kDeviceContainerPrimaryCategoryPropKey));

        if (hints.bluetooth_cod_major.has_value() || !hints.device_categories.empty()) {
            return hints;
        }
    }

    return std::nullopt;
}

std::optional<std::uint8_t> ReadPhoneHfpBatteryHintFromPnpAddress(std::uint64_t address,
                                                                  bool debug_enabled,
                                                                  XiaomiDebugLogFn debug_log) {
    if (address <= 0xFFFFULL) {
        return std::nullopt;
    }

    std::vector<std::wstring> instance_ids;
    std::unordered_set<std::wstring> seen_instance_ids;
    auto append_instance_ids = [&](const std::vector<std::wstring>& ids) {
        for (const auto& instance_id : ids) {
            std::wstring normalized = instance_id;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t value) {
                return static_cast<wchar_t>(std::towupper(value));
            });
            if (seen_instance_ids.insert(std::move(normalized)).second) {
                instance_ids.push_back(instance_id);
            }
        }
    };

    constexpr std::array<const wchar_t*, 3> kServiceUuids = {
        L"0000111F-0000-1000-8000-00805F9B34FB",
        L"0000111E-0000-1000-8000-00805F9B34FB",
        L"00001112-0000-1000-8000-00805F9B34FB",
    };
    for (const auto* service_uuid : kServiceUuids) {
        append_instance_ids(FindBthEnumServiceInstanceIdsByAddress(address, service_uuid));
    }
    append_instance_ids(FindBthEnumInstanceIdsByAddress(address));

    if (instance_ids.empty()) {
        LogDebug(debug_enabled, debug_log,
                 "Phone PnP battery hint: instance id not found for address=" + std::to_string(address));
        return std::nullopt;
    }

    auto read_property = [&](DEVINST dev_inst, const DEVPROPKEY& property_key) -> std::optional<std::uint8_t> {
        DEVPROPTYPE property_type = DEVPROP_TYPE_EMPTY;
        const auto raw_data = ReadDevNodePropertyRaw(dev_inst, property_key, &property_type);
        if (!raw_data.has_value()) {
            return std::nullopt;
        }

        if (debug_enabled) {
            const std::size_t preview_len = std::min<std::size_t>(raw_data->size(), 24U);
            std::vector<std::uint8_t> preview(raw_data->begin(),
                                              raw_data->begin() + static_cast<std::ptrdiff_t>(preview_len));
            LogDebug(debug_enabled, debug_log,
                     "Phone PnP raw pid=" + std::to_string(property_key.pid) +
                         " type=" + std::to_string(property_type) +
                         " size=" + std::to_string(raw_data->size()) +
                         " data=" + BytesToHex(preview));
        }

        if (property_type == DEVPROP_TYPE_BYTE && !raw_data->empty()) {
            return NormalizePhoneBatteryHintScalar(static_cast<int>((*raw_data)[0]));
        }
        if (property_type == DEVPROP_TYPE_UINT16 && raw_data->size() >= sizeof(std::uint16_t)) {
            std::uint16_t value = 0;
            std::memcpy(&value, raw_data->data(), sizeof(value));
            return NormalizePhoneBatteryHintScalar(static_cast<int>(value));
        }
        if (property_type == DEVPROP_TYPE_INT16 && raw_data->size() >= sizeof(std::int16_t)) {
            std::int16_t value = 0;
            std::memcpy(&value, raw_data->data(), sizeof(value));
            return NormalizePhoneBatteryHintScalar(static_cast<int>(value));
        }
        if (property_type == DEVPROP_TYPE_UINT32 && raw_data->size() >= sizeof(std::uint32_t)) {
            std::uint32_t value = 0;
            std::memcpy(&value, raw_data->data(), sizeof(value));
            if (value <= static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
                return NormalizePhoneBatteryHintScalar(static_cast<int>(value));
            }
            return std::nullopt;
        }
        if (property_type == DEVPROP_TYPE_INT32 && raw_data->size() >= sizeof(std::int32_t)) {
            std::int32_t value = 0;
            std::memcpy(&value, raw_data->data(), sizeof(value));
            return NormalizePhoneBatteryHintScalar(static_cast<int>(value));
        }
        if ((property_type & DEVPROP_MASK_TYPE) == DEVPROP_TYPE_STRING && raw_data->size() >= sizeof(wchar_t)) {
            std::wstring text(reinterpret_cast<const wchar_t*>(raw_data->data()),
                              raw_data->size() / sizeof(wchar_t));
            if (const auto null_pos = text.find(L'\0'); null_pos != std::wstring::npos) {
                text.resize(null_pos);
            }

            const wchar_t* cursor = text.c_str();
            while (*cursor != L'\0') {
                while (*cursor != L'\0' && !std::iswdigit(*cursor) && *cursor != L'-') {
                    ++cursor;
                }
                if (*cursor == L'\0') {
                    break;
                }
                wchar_t* end = nullptr;
                const long value = std::wcstol(cursor, &end, 10);
                if (end != cursor) {
                    return NormalizePhoneBatteryHintScalar(static_cast<int>(value));
                }
                cursor = end;
            }
        }

        return std::nullopt;
    };

    for (const auto& instance_id : instance_ids) {
        DEVINST dev_inst = 0;
        const auto locate_result =
            CM_Locate_DevNodeW(&dev_inst, const_cast<wchar_t*>(instance_id.c_str()), CM_LOCATE_DEVNODE_NORMAL);
        if (locate_result != CR_SUCCESS) {
            continue;
        }

        std::vector<DEVPROPKEY> keys_to_probe = {kPhoneHfpBatteryHintPropKey};
        ULONG key_count = 0;
        const auto key_count_result = CM_Get_DevNode_Property_Keys(dev_inst, nullptr, &key_count, 0);
        if ((key_count_result == CR_SUCCESS || key_count_result == CR_BUFFER_SMALL) && key_count > 0U) {
            std::vector<DEVPROPKEY> property_keys(key_count);
            if (CM_Get_DevNode_Property_Keys(dev_inst, property_keys.data(), &key_count, 0) == CR_SUCCESS) {
                property_keys.resize(key_count);
                for (const auto& property_key : property_keys) {
                    if (std::memcmp(&property_key.fmtid, &kPhoneHfpBatteryHintPropKey.fmtid, sizeof(GUID)) != 0) {
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

        for (const auto& property_key : keys_to_probe) {
            const auto value = read_property(dev_inst, property_key);
            if (!value.has_value()) {
                continue;
            }
            LogDebug(debug_enabled, debug_log,
                     "Phone PnP battery hint accepted value=" + std::to_string(*value) +
                         " pid=" + std::to_string(property_key.pid) +
                         " instance='" + ToUtf8(instance_id) + "'");
            return value;
        }
    }

    LogDebug(debug_enabled, debug_log,
             "Phone PnP battery hint was not found for address=" + std::to_string(address));
    return std::nullopt;
}

std::optional<std::uint8_t> ReadZmiVendorBatteryHintFromPnpAddress(std::uint64_t address,
                                                                    bool debug_enabled,
                                                                    XiaomiDebugLogFn debug_log) {
    if (address <= 0xFFFFULL) {
        return std::nullopt;
    }

    std::vector<std::wstring> instance_ids;
    std::unordered_set<std::wstring> seen_instance_ids;
    auto append_instance_ids = [&](const std::vector<std::wstring>& ids) {
        for (const auto& instance_id : ids) {
            std::wstring normalized = instance_id;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t value) {
                return static_cast<wchar_t>(std::towupper(value));
            });
            if (seen_instance_ids.insert(std::move(normalized)).second) {
                instance_ids.push_back(instance_id);
            }
        }
    };

    append_instance_ids(FindBthEnumInstanceIdsByAddress(address));
    append_instance_ids(FindBthLeInstanceIdsByAddress(address));

    if (instance_ids.empty()) {
        LogDebug(debug_enabled, debug_log,
                 "ZMI PnP battery hint: instance id not found for address=" + std::to_string(address));
        return std::nullopt;
    }

    for (const auto& instance_id : instance_ids) {
        DEVINST dev_inst = 0;
        const auto locate_result =
            CM_Locate_DevNodeW(&dev_inst, const_cast<wchar_t*>(instance_id.c_str()), CM_LOCATE_DEVNODE_NORMAL);
        if (locate_result != CR_SUCCESS) {
            continue;
        }

        // Try the known ZMI vendor battery property key first.
        std::vector<DEVPROPKEY> keys_to_probe = {kZmiVendorBatteryHintPropKey};

        // Also scan all properties under the same GUID family for other PIDs.
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

        for (const auto& property_key : keys_to_probe) {
            DEVPROPTYPE property_type = DEVPROP_TYPE_EMPTY;
            const auto raw_data = ReadDevNodePropertyRaw(dev_inst, property_key, &property_type);
            if (!raw_data.has_value()) {
                continue;
            }

            if (debug_enabled) {
                const std::size_t preview_len = std::min<std::size_t>(raw_data->size(), 24U);
                std::vector<std::uint8_t> preview(raw_data->begin(),
                                                  raw_data->begin() + static_cast<std::ptrdiff_t>(preview_len));
                LogDebug(debug_enabled, debug_log,
                         "ZMI PnP raw pid=" + std::to_string(property_key.pid) +
                             " type=" + std::to_string(property_type) +
                             " size=" + std::to_string(raw_data->size()) +
                             " data=" + BytesToHex(preview));
            }

            const auto value = NormalizePhoneBatteryHintScalar(
                [&]() -> int {
                    if (property_type == DEVPROP_TYPE_BYTE && !raw_data->empty()) {
                        return static_cast<int>((*raw_data)[0]);
                    }
                    if (property_type == DEVPROP_TYPE_UINT32 && raw_data->size() >= sizeof(std::uint32_t)) {
                        std::uint32_t v = 0;
                        std::memcpy(&v, raw_data->data(), sizeof(v));
                        return static_cast<int>(v);
                    }
                    if (property_type == DEVPROP_TYPE_INT32 && raw_data->size() >= sizeof(std::int32_t)) {
                        std::int32_t v = 0;
                        std::memcpy(&v, raw_data->data(), sizeof(v));
                        return static_cast<int>(v);
                    }
                    return -1;
                }());
            if (!value.has_value()) {
                continue;
            }

            LogDebug(debug_enabled, debug_log,
                     "ZMI PnP battery hint accepted value=" + std::to_string(*value) +
                         " pid=" + std::to_string(property_key.pid) +
                         " instance='" + ToUtf8(instance_id) + "'");
            return value;
        }
    }

    LogDebug(debug_enabled, debug_log,
             "ZMI PnP battery hint was not found for address=" + std::to_string(address));
    return std::nullopt;
}

}  // namespace battery_monitor

