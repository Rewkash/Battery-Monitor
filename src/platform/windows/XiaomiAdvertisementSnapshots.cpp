#include "platform/windows/XiaomiAdvertisementSnapshots.h"

#include <algorithm>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include "platform/windows/ZmiBatteryCodec.h"

namespace battery_monitor {

namespace {

using winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs;
using winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher;
using winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEScanningMode;
using winrt::Windows::Storage::Streams::DataReader;

struct ScoredXiaomiSnapshot {
    XiaomiBatterySnapshot snapshot;
    int score = -1;
};

void LogDebug(XiaomiDebugLogFn debug_log, const std::string& message) {
    if (debug_log != nullptr) {
        debug_log(message);
    }
}

std::string ToUtf8(const winrt::hstring& value) {
    return winrt::to_string(value);
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::vector<std::uint8_t> ReadBufferBytes(const winrt::Windows::Storage::Streams::IBuffer& buffer) {
    const auto reader = DataReader::FromBuffer(buffer);
    std::vector<std::uint8_t> bytes(reader.UnconsumedBufferLength());
    if (!bytes.empty()) {
        reader.ReadBytes(bytes);
    }
    return bytes;
}

std::optional<ScoredXiaomiSnapshot> DecodeXiaomiSnapshotFromAdvertisementPayload(
    const std::vector<std::uint8_t>& payload,
    bool debug_enabled,
    XiaomiDebugLogFn debug_log) {
    if (payload.empty()) {
        return std::nullopt;
    }

    std::optional<ScoredXiaomiSnapshot> best_candidate;

    auto score_snapshot = [](const XiaomiBatterySnapshot& snapshot, int base_score) {
        int score = base_score;
        const int presence = XiaomiBatteryPresenceCount(snapshot);
        score += presence * 10;
        if (presence == 3) {
            score += 8;
        }
        if (snapshot.left.has_value() && snapshot.right.has_value() && *snapshot.left != *snapshot.right) {
            score += 5;
        }
        if ((snapshot.left.has_value() && *snapshot.left == 100U) ||
            (snapshot.right.has_value() && *snapshot.right == 100U) ||
            (snapshot.case_level.has_value() && *snapshot.case_level == 100U)) {
            score += 2;
        }
        return score;
    };

    auto consider = [&](const XiaomiBatterySnapshot& snapshot, int base_score) {
        const int presence = XiaomiBatteryPresenceCount(snapshot);
        if (presence < 2) {
            return;
        }
        const int score = score_snapshot(snapshot, base_score);
        if (!best_candidate.has_value() || score > best_candidate->score) {
            best_candidate = ScoredXiaomiSnapshot{snapshot, score};
        }
    };

    if (const auto parsed =
            ExtractBatterySnapshotFromXiaomiPayload(payload, std::nullopt, debug_enabled, debug_log);
        parsed.has_value()) {
        consider(*parsed, 64);
    }

    if (const auto parsed = ExtractZmiSerialPatternSnapshot(payload); parsed.has_value()) {
        consider(*parsed, 74);
    }

    for (std::size_t index = 0; index + 4U < payload.size(); ++index) {
        if (payload[index] != 0x04U || payload[index + 1U] > 0x7FU) {
            continue;
        }

        XiaomiBatterySnapshot snapshot;
        snapshot.left = ParseXiaomiBatteryRaw(payload[index + 2U]);
        snapshot.right = ParseXiaomiBatteryRaw(payload[index + 3U]);
        snapshot.case_level = ParseXiaomiBatteryRaw(payload[index + 4U]);
        consider(snapshot, 56);
    }

    for (std::size_t index = 0; index + 2U < payload.size(); ++index) {
        const std::uint8_t left_raw = payload[index];
        const std::uint8_t right_raw = payload[index + 1U];
        const std::uint8_t case_raw = payload[index + 2U];

        const int plausible_raw_count =
            (left_raw <= 100U || left_raw == 0xFFU ? 1 : 0) +
            (right_raw <= 100U || right_raw == 0xFFU ? 1 : 0) +
            (case_raw <= 100U || case_raw == 0xFFU ? 1 : 0);
        if (plausible_raw_count < 2) {
            continue;
        }

        XiaomiBatterySnapshot snapshot;
        snapshot.left = ParseXiaomiBatteryRaw(left_raw);
        snapshot.right = ParseXiaomiBatteryRaw(right_raw);
        snapshot.case_level = ParseXiaomiBatteryRaw(case_raw);
        consider(snapshot, 38);
    }

    if (!best_candidate.has_value() || best_candidate->score < 58) {
        return std::nullopt;
    }

    return best_candidate;
}

std::uint64_t ComposeBluetoothAddressFromBytes(const std::uint8_t* bytes, bool reverse_order) {
    if (bytes == nullptr) {
        return 0ULL;
    }

    std::uint64_t address = 0ULL;
    for (std::size_t index = 0; index < 6U; ++index) {
        const std::size_t source_index = reverse_order ? (5U - index) : index;
        address = (address << 8U) | static_cast<std::uint64_t>(bytes[source_index]);
    }
    return address;
}

std::vector<std::uint64_t> ParseXiaomiAddressAliasesFromPayload(const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint64_t> aliases;
    if (payload.size() < 11U) {
        return aliases;
    }

    const std::uint16_t frame_control = static_cast<std::uint16_t>(
        payload[0] | (static_cast<std::uint16_t>(payload[1]) << 8U));
    constexpr std::uint16_t kFrameControlMacIncluded = 0x0010U;
    if ((frame_control & kFrameControlMacIncluded) == 0U) {
        return aliases;
    }

    const auto* mac_bytes = payload.data() + 5U;
    auto append_alias = [&](std::uint64_t address) {
        if (address <= 0xFFFFULL || address == 0ULL || address == 0xFFFFFFFFFFFFULL) {
            return;
        }
        if (std::find(aliases.begin(), aliases.end(), address) != aliases.end()) {
            return;
        }
        aliases.push_back(address);
    };

    append_alias(ComposeBluetoothAddressFromBytes(mac_bytes, false));
    append_alias(ComposeBluetoothAddressFromBytes(mac_bytes, true));

    return aliases;
}

std::optional<std::uint16_t> ParseXiaomiProductIdFromPayload(const std::vector<std::uint8_t>& payload) {
    if (payload.size() < 4U) {
        return std::nullopt;
    }

    const std::uint16_t frame_control = static_cast<std::uint16_t>(
        payload[0] | (static_cast<std::uint16_t>(payload[1]) << 8U));
    const std::uint16_t product_id = static_cast<std::uint16_t>(
        payload[2] | (static_cast<std::uint16_t>(payload[3]) << 8U));

    if (product_id == 0U || product_id == 0xFFFFU) {
        return std::nullopt;
    }
    if (frame_control == 0U) {
        return std::nullopt;
    }

    return product_id;
}

void StoreBestAdvertisementSnapshot(std::unordered_map<std::uint64_t, ScoredXiaomiSnapshot>* snapshots,
                                    std::uint64_t address,
                                    const XiaomiBatterySnapshot& snapshot,
                                    int score) {
    if (snapshots == nullptr || address <= 0xFFFFULL) {
        return;
    }

    auto found = snapshots->find(address);
    if (found == snapshots->end() || score > found->second.score) {
        (*snapshots)[address] = ScoredXiaomiSnapshot{snapshot, score};
    }
}

void StoreBestAdvertisementSnapshotByName(std::unordered_map<std::string, ScoredXiaomiSnapshot>* snapshots,
                                          const std::string& normalized_name,
                                          const XiaomiBatterySnapshot& snapshot,
                                          int score) {
    if (snapshots == nullptr || normalized_name.empty()) {
        return;
    }

    auto found = snapshots->find(normalized_name);
    if (found == snapshots->end() || score > found->second.score) {
        (*snapshots)[normalized_name] = ScoredXiaomiSnapshot{snapshot, score};
    }
}

void StoreBestAdvertisementSnapshotByProductId(
    std::unordered_map<std::uint16_t, ScoredXiaomiSnapshot>* snapshots,
    std::uint16_t product_id,
    const XiaomiBatterySnapshot& snapshot,
    int score) {
    if (snapshots == nullptr || product_id == 0U || product_id == 0xFFFFU) {
        return;
    }

    auto found = snapshots->find(product_id);
    if (found == snapshots->end() || score > found->second.score) {
        (*snapshots)[product_id] = ScoredXiaomiSnapshot{snapshot, score};
    }
}

}  // namespace

AdvertisementSnapshotResult ScanXiaomiAdvertisementSnapshots(std::chrono::milliseconds scan_duration,
                                                             OpenBleDeviceByAddressFn open_ble_device,
                                                             bool debug_enabled,
                                                             XiaomiDebugLogFn debug_log) {
    AdvertisementSnapshotResult snapshots;

    try {
        BluetoothLEAdvertisementWatcher watcher;
        watcher.ScanningMode(BluetoothLEScanningMode::Active);

        std::mutex snapshots_mutex;
        std::unordered_map<std::uint64_t, ScoredXiaomiSnapshot> scored_snapshots;
        std::unordered_map<std::string, ScoredXiaomiSnapshot> scored_name_snapshots;
        std::unordered_map<std::uint16_t, ScoredXiaomiSnapshot> scored_pid_snapshots;

        const auto received_token = watcher.Received(
            [&snapshots_mutex, &scored_snapshots, &scored_name_snapshots, &scored_pid_snapshots, debug_enabled, debug_log](
                const BluetoothLEAdvertisementWatcher&,
                const BluetoothLEAdvertisementReceivedEventArgs& args) {
                const auto bluetooth_address = args.BluetoothAddress();
                if (bluetooth_address <= 0xFFFFULL) {
                    return;
                }

                const auto advertisement = args.Advertisement();
                const std::string local_name = ToLowerAscii(ToUtf8(advertisement.LocalName()));
                for (const auto& manufacturer : advertisement.ManufacturerData()) {
                    const auto company_id = manufacturer.CompanyId();
                    if (company_id != 0x038FU && company_id != 0x2717U) {
                        continue;
                    }
                    const auto payload = ReadBufferBytes(manufacturer.Data());
                    const auto decoded =
                        DecodeXiaomiSnapshotFromAdvertisementPayload(payload, debug_enabled, debug_log);
                    if (!decoded.has_value()) {
                        continue;
                    }
                    const auto product_id = ParseXiaomiProductIdFromPayload(payload);
                    const auto address_aliases = ParseXiaomiAddressAliasesFromPayload(payload);

                    const int score = decoded->score + 18;
                    std::scoped_lock lock(snapshots_mutex);
                    StoreBestAdvertisementSnapshot(&scored_snapshots, bluetooth_address, decoded->snapshot, score);
                    for (const auto alias : address_aliases) {
                        StoreBestAdvertisementSnapshot(&scored_snapshots, alias, decoded->snapshot, score + 6);
                    }
                    StoreBestAdvertisementSnapshotByName(&scored_name_snapshots, local_name, decoded->snapshot, score);
                    if (product_id.has_value()) {
                        StoreBestAdvertisementSnapshotByProductId(
                            &scored_pid_snapshots, *product_id, decoded->snapshot, score + 8);
                    }
                }

                for (const auto& section : advertisement.DataSections()) {
                    const auto section_bytes = ReadBufferBytes(section.Data());
                    if (section.DataType() != 0x16U || section_bytes.size() < 2U) {
                        continue;
                    }

                    const std::uint16_t service_uuid = static_cast<std::uint16_t>(
                        section_bytes[0] | (static_cast<std::uint16_t>(section_bytes[1]) << 8U));
                    if (service_uuid != 0xFD2DU && service_uuid != 0xFE95U) {
                        continue;
                    }

                    std::vector<std::uint8_t> payload(section_bytes.begin() + 2, section_bytes.end());
                    const auto product_id = ParseXiaomiProductIdFromPayload(payload);
                    const auto decoded =
                        DecodeXiaomiSnapshotFromAdvertisementPayload(payload, debug_enabled, debug_log);
                    if (!decoded.has_value()) {
                        continue;
                    }
                    const auto address_aliases = ParseXiaomiAddressAliasesFromPayload(payload);

                    const int score = decoded->score + 20;
                    std::scoped_lock lock(snapshots_mutex);
                    StoreBestAdvertisementSnapshot(&scored_snapshots, bluetooth_address, decoded->snapshot, score);
                    for (const auto alias : address_aliases) {
                        StoreBestAdvertisementSnapshot(&scored_snapshots, alias, decoded->snapshot, score + 8);
                    }
                    StoreBestAdvertisementSnapshotByName(&scored_name_snapshots, local_name, decoded->snapshot, score);
                    if (product_id.has_value()) {
                        StoreBestAdvertisementSnapshotByProductId(
                            &scored_pid_snapshots, *product_id, decoded->snapshot, score + 10);
                    }
                }
            });

        watcher.Start();
        std::this_thread::sleep_for(scan_duration);
        watcher.Stop();
        watcher.Received(received_token);

        std::scoped_lock lock(snapshots_mutex);
        snapshots.by_address.reserve(scored_snapshots.size());
        for (const auto& [address, scored] : scored_snapshots) {
            snapshots.by_address[address] = scored.snapshot;
            if (debug_enabled) {
                const auto readings = BuildXiaomiBatteryReadings(scored.snapshot);
                LogDebug(debug_log,
                         "BLE advertisement snapshot address=" + std::to_string(address) +
                             " entries=" + std::to_string(readings.size()) +
                             " score=" + std::to_string(scored.score));
            }
        }

        if (open_ble_device != nullptr) {
            for (const auto& [address, scored] : scored_snapshots) {
                const auto maybe_ble_device = open_ble_device(address, std::chrono::milliseconds(1200));
                if (!maybe_ble_device.has_value() || !(*maybe_ble_device)) {
                    continue;
                }

                const std::string resolved_name = ToLowerAscii(ToUtf8((*maybe_ble_device).Name()));
                if (resolved_name.empty()) {
                    continue;
                }
                StoreBestAdvertisementSnapshotByName(
                    &scored_name_snapshots, resolved_name, scored.snapshot, scored.score + 6);
                if (debug_enabled) {
                    LogDebug(debug_log,
                             "BLE advertisement snapshot resolved name='" + resolved_name +
                                 "' from address=" + std::to_string(address));
                }
            }
        }

        snapshots.by_name.reserve(scored_name_snapshots.size());
        for (const auto& [name, scored] : scored_name_snapshots) {
            snapshots.by_name[name] = scored.snapshot;
            if (debug_enabled) {
                const auto readings = BuildXiaomiBatteryReadings(scored.snapshot);
                LogDebug(debug_log,
                         "BLE advertisement snapshot name='" + name + "'" +
                             " entries=" + std::to_string(readings.size()) +
                             " score=" + std::to_string(scored.score));
            }
        }
        snapshots.by_product_id.reserve(scored_pid_snapshots.size());
        for (const auto& [product_id, scored] : scored_pid_snapshots) {
            snapshots.by_product_id[product_id] = scored.snapshot;
            if (debug_enabled) {
                const auto readings = BuildXiaomiBatteryReadings(scored.snapshot);
                std::ostringstream pid_stream;
                pid_stream << "0x" << std::uppercase << std::hex
                           << std::setw(4) << std::setfill('0') << product_id;
                LogDebug(debug_log,
                         "BLE advertisement snapshot productId=" + pid_stream.str() +
                             " entries=" + std::to_string(readings.size()) +
                             " score=" + std::to_string(scored.score));
            }
        }
    } catch (const winrt::hresult_error& error) {
        const auto message = winrt::to_string(error.message());
        LogDebug(debug_log,
                 "BLE advertisement fallback failed: HRESULT=0x" +
                     [&]() {
                         std::ostringstream stream;
                         stream << std::uppercase << std::hex
                                << static_cast<std::uint32_t>(error.code().value) << std::dec;
                         if (!message.empty()) {
                             stream << " message='" << message << "'";
                         }
                         return stream.str();
                     }());
    } catch (const std::exception& error) {
        LogDebug(debug_log, std::string("BLE advertisement fallback failed: ") + error.what());
    }

    return snapshots;
}

}  // namespace battery_monitor
