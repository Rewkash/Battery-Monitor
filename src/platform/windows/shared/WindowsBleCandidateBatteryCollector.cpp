#include "platform/windows/shared/WindowsBleCandidateBatteryCollector.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include "platform/windows/bluetooth/BleCandidateEnumeration.h"
#include "platform/windows/bluetooth/BleStandardBatteryReader.h"
#include "platform/windows/bluetooth/BleVendorTripletReader.h"
#include "platform/windows/shared/WindowsBatteryEntryUtils.h"
#include "platform/windows/shared/WindowsBatteryProviderSupport.h"
#include "platform/windows/shared/WindowsBluetoothAddressUtils.h"
#include "platform/windows/shared/WindowsDeviceInfoProperties.h"
#include "platform/windows/devices/xiaomi/XiaomiBatteryReadings.h"

namespace battery_monitor {

namespace {

using winrt::Windows::Devices::Bluetooth::BluetoothConnectionStatus;
using winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
using winrt::Windows::Foundation::AsyncStatus;

constexpr auto kEmptyBleCandidateBackoff = std::chrono::seconds(20);

std::mutex g_empty_ble_candidate_mutex;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_empty_ble_candidate_until;

std::string ToUtf8(const winrt::hstring& value) {
    return winrt::to_string(value);
}

std::string ToLowerAscii(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

bool IsLikelyZmiPurpodsProbe(const std::string& value) {
    const std::string lowered = ToLowerAscii(value);
    return lowered.find("zmi") != std::string::npos || lowered.find("purpods") != std::string::npos;
}

void LogDebug(XiaomiDebugLogFn debug_log, const std::string& message) {
    if (debug_log != nullptr) {
        debug_log(message);
    }
}

bool ShouldSkipRecentEmptyBleCandidate(const std::string& candidate_id) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_empty_ble_candidate_mutex);
    const auto known = g_empty_ble_candidate_until.find(candidate_id);
    if (known == g_empty_ble_candidate_until.end()) {
        return false;
    }
    if (known->second <= now) {
        g_empty_ble_candidate_until.erase(known);
        return false;
    }
    return true;
}

void RememberEmptyBleCandidate(const std::string& candidate_id) {
    std::lock_guard<std::mutex> lock(g_empty_ble_candidate_mutex);
    g_empty_ble_candidate_until[candidate_id] = std::chrono::steady_clock::now() + kEmptyBleCandidateBackoff;
}

void ForgetEmptyBleCandidate(const std::string& candidate_id) {
    std::lock_guard<std::mutex> lock(g_empty_ble_candidate_mutex);
    g_empty_ble_candidate_until.erase(candidate_id);
}

template <typename TResult>
std::optional<TResult> WaitForAsyncResult(winrt::Windows::Foundation::IAsyncOperation<TResult> operation,
                                          std::chrono::milliseconds timeout) {
    try {
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (operation.Status() == AsyncStatus::Started && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        if (operation.Status() == AsyncStatus::Started) {
            operation.Cancel();
            return std::nullopt;
        }

        if (operation.Status() != AsyncStatus::Completed) {
            return std::nullopt;
        }

        return operation.GetResults();
    } catch (const winrt::hresult_error&) {
        return std::nullopt;
    }
}

}  // namespace

void CollectBleCandidateBatteryEntries(const WindowsBleCandidateBatteryCollectorContext& context,
                                       DeviceBatteryAccumulator* accumulator,
                                       XiaomiClassicBatteryCache* xiaomi_classic_cache) {
    if (accumulator == nullptr || xiaomi_classic_cache == nullptr) {
        return;
    }

    const auto device_infos = EnumerateBleCandidateDevices(context.debug_enabled, context.debug_log);
    LogDebug(context.debug_log, "BLE candidates from selectors: " + std::to_string(device_infos.size()));

    for (const auto& device_info : device_infos) {
        try {
            const std::string candidate_id = ToUtf8(device_info.Id());
            if (!context.target_device_id.empty() &&
                !DeviceIdMatchesBluetoothTarget(candidate_id, context.target_device_id)) {
                continue;
            }
            if (ShouldSkipRecentEmptyBleCandidate(candidate_id)) {
                if (context.debug_enabled) {
                    LogDebug(context.debug_log,
                             "BLE candidate skipped after recent empty read id='" + candidate_id + "'");
                }
                continue;
            }
            if (context.debug_enabled) {
                LogDebug(context.debug_log, "BLE candidate begin id='" + candidate_id + "'");
            }

            const auto open_started_at = std::chrono::steady_clock::now();
            const auto maybe_ble_device =
                WaitForAsyncResult(BluetoothLEDevice::FromIdAsync(device_info.Id()), std::chrono::milliseconds(1200));
            if (!maybe_ble_device.has_value() || !(*maybe_ble_device)) {
                if (context.debug_enabled) {
                    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - open_started_at);
                    LogDebug(context.debug_log,
                             "BLE candidate open failed in " + std::to_string(elapsed_ms.count()) +
                                 " ms id='" + candidate_id + "'");
                }
                continue;
            }
            const auto ble_device = *maybe_ble_device;
            const bool ble_is_connected = ble_device.ConnectionStatus() == BluetoothConnectionStatus::Connected;
            if (!ble_is_connected) {
                if (context.debug_enabled) {
                    LogDebug(context.debug_log,
                             "BLE candidate skipped because device is disconnected id='" + candidate_id + "'");
                }
                continue;
            }
            if (context.debug_enabled) {
                const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - open_started_at);
                LogDebug(context.debug_log,
                         "BLE candidate open succeeded in " + std::to_string(elapsed_ms.count()) +
                             " ms id='" + candidate_id + "'");
            }

            std::string device_id = candidate_id;
            std::string device_name = ToUtf8(device_info.Name());
            const std::string ble_name = ToUtf8(ble_device.Name());
            if (device_name.empty()) {
                device_name = ble_name;
            }
            if (device_name.empty()) {
                device_name = "Unknown";
            }

            const bool likely_tws =
                context.is_likely_tws_device != nullptr &&
                context.is_likely_tws_device(device_name, ble_name, device_id);
            const std::string lowered_probe = ToLowerAscii(device_name + " " + ble_name + " " + device_id);
            const bool likely_zmi = IsLikelyZmiPurpodsProbe(lowered_probe);
            const bool target_is_likely_zmi = IsLikelyZmiPurpodsProbe(context.target_device_id);
            const bool likely_xiaomi_tws =
                likely_tws &&
                (likely_zmi || !target_is_likely_zmi) &&
                context.is_likely_xiaomi_earbuds != nullptr &&
                context.is_likely_xiaomi_earbuds(device_name, ble_name, device_id);
            if (context.debug_enabled) {
                if (lowered_probe.find("redmi") != std::string::npos ||
                    lowered_probe.find("buds") != std::string::npos ||
                    lowered_probe.find("zmi") != std::string::npos ||
                    lowered_probe.find("purpods") != std::string::npos) {
                    const auto ble_address = TryGetBluetoothAddress(ble_device);
                    LogDebug(context.debug_log,
                             "BLE device opened: name='" + device_name + "' bleName='" + ble_name + "' id='" + device_id +
                                 "' tws=" + (likely_tws ? "true" : "false") +
                                 " xiaomiTws=" + (likely_xiaomi_tws ? "true" : "false") +
                                  " address=" + (ble_address.has_value() ? std::to_string(*ble_address) : "n/a"));
                }
            }

            if (context.debug_enabled) {
                CaptureBleNotifyDebugSnapshot(
                    ble_device,
                    std::chrono::milliseconds(1600),
                    context.debug_enabled,
                    context.debug_log);
                if (likely_zmi) {
                    CaptureBleGattLayoutDebugSnapshot(
                        ble_device,
                        context.debug_enabled,
                        context.debug_log);
                }
            }

            std::vector<BatteryReading> battery_readings;
            if (!likely_xiaomi_tws) {
                const auto read_started_at = std::chrono::steady_clock::now();
                battery_readings = ReadBleBatteryReadings(ble_device, likely_tws);
                if (context.debug_enabled) {
                    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - read_started_at);
                    LogDebug(context.debug_log,
                             "BLE candidate battery read took " + std::to_string(elapsed_ms.count()) +
                                 " ms, entries=" + std::to_string(battery_readings.size()) +
                                 " id='" + device_id + "'");
                }
            } else {
                LogDebug(context.debug_log, "BLE candidate: Xiaomi-family TWS, skip standard battery service read");
            }

            std::vector<BatteryReading> resolved_readings = battery_readings;
            bool resolved_from_persistent_cache = false;
            std::optional<std::uint64_t> resolved_address_for_fallback = TryGetBluetoothAddress(ble_device);
            if (!resolved_address_for_fallback.has_value()) {
                resolved_address_for_fallback = ParseBluetoothAddressFromDeviceId(device_id);
            }

            if (likely_xiaomi_tws && !HasUsefulXiaomiTwsReadings(resolved_readings)) {
                if (resolved_address_for_fallback.has_value()) {
                    const auto& classic_result =
                        xiaomi_classic_cache->Read(*resolved_address_for_fallback,
                                                   likely_zmi ? ClassicBatteryService::kZmiPurPodsSerial
                                                              : ClassicBatteryService::kXiaomiDeviceControl,
                                                    context.force_live_refresh,
                                                   2U);
                    if (!classic_result.readings.empty()) {
                        resolved_readings = classic_result.readings;
                        resolved_from_persistent_cache = classic_result.from_persistent_cache;
                    }
                } else {
                    LogDebug(context.debug_log,
                             "BLE Xiaomi fallback skipped because address could not be resolved for '" + device_name + "'");
                }
            }

            if (resolved_readings.empty()) {
                const auto fff1_readings = TryReadBleFff1Battery(ble_device, context.debug_enabled, context.debug_log);
                if (!fff1_readings.empty()) {
                    resolved_readings = fff1_readings;
                    resolved_from_persistent_cache = false;
                    LogDebug(context.debug_log, "BLE FFF1 fallback accepted for '" + device_name + "'");
                }
            }

            if (likely_tws && resolved_readings.size() <= 1U) {
                const auto vendor_triplet = TryReadBleVendorTripletBattery(ble_device, context.debug_enabled, context.debug_log);
                if (vendor_triplet.size() >= 2U) {
                    resolved_readings = vendor_triplet;
                    resolved_from_persistent_cache = false;
                    LogDebug(context.debug_log, "BLE vendor triplet fallback accepted for '" + device_name + "'");
                }
            }

            if (likely_xiaomi_tws &&
                !HasUsefulXiaomiTwsReadings(resolved_readings)) {
                const auto late_standard_readings = ReadBleBatteryReadings(ble_device, likely_tws);
                if (XiaomiReadingsRichnessScore(late_standard_readings) > XiaomiReadingsRichnessScore(resolved_readings)) {
                    resolved_readings = late_standard_readings;
                    resolved_from_persistent_cache = false;
                    if (!resolved_readings.empty()) {
                        LogDebug(context.debug_log,
                                 "BLE Xiaomi fallback: standard battery service yielded " +
                                     std::to_string(resolved_readings.size()) +
                                     " entries for '" + device_name + "'");
                    }
                }
            }

            if (!likely_tws && resolved_readings.empty()) {
                const auto endpoint_battery = ReadBatteryPercentFromEndpointProperties(device_info);
                if (endpoint_battery.has_value()) {
                    resolved_readings.push_back(BatteryReading{"main", *endpoint_battery});
                    resolved_from_persistent_cache = false;
                    if (context.debug_enabled) {
                        LogDebug(context.debug_log,
                                 "BLE endpoint property fallback accepted for '" + device_name +
                                     "': " + std::to_string(*endpoint_battery));
                    }
                }
            }

            if (resolved_readings.empty()) {
                if (context.debug_enabled) {
                    const std::string lowered_probe_missing = ToLowerAscii(device_name + " " + ble_name + " " + device_id);
                    if (lowered_probe_missing.find("redmi") != std::string::npos ||
                        lowered_probe_missing.find("buds") != std::string::npos ||
                        lowered_probe_missing.find("zmi") != std::string::npos ||
                        lowered_probe_missing.find("purpods") != std::string::npos) {
                        LogDebug(context.debug_log, "BLE battery not found for '" + device_name + "'");
                    }
                }

                if (likely_tws || ble_is_connected) {
                    DeviceBatteryInfo unknown_entry;
                    unknown_entry.device_id = device_id;
                    unknown_entry.device_name = device_name;
                    unknown_entry.battery_component = "main";
                    unknown_entry.battery_level_percent = std::nullopt;
                    PopulateBluetoothVisualHintsFromDeviceInfo(device_info, &unknown_entry);
                    unknown_entry.is_connected = ble_is_connected;
                    accumulator->AddEntry(std::move(unknown_entry));
                }
                RememberEmptyBleCandidate(candidate_id);
                continue;
            }

            ForgetEmptyBleCandidate(candidate_id);

            if (likely_xiaomi_tws &&
                resolved_address_for_fallback.has_value() &&
                !resolved_from_persistent_cache &&
                HasUsefulXiaomiTwsReadings(resolved_readings, 2U)) {
                xiaomi_classic_cache->Persist(*resolved_address_for_fallback, resolved_readings);
            }

            const bool has_tws_components = std::any_of(
                resolved_readings.begin(), resolved_readings.end(), [](const BatteryReading& reading) {
                    const std::string component = ToLowerAscii(reading.component);
                    return component == "left" || component == "right" || component == "case";
                });
            if (likely_tws && !resolved_from_persistent_cache && has_tws_components &&
                resolved_address_for_fallback.has_value()) {
                std::unordered_set<std::string> tws_components;
                for (const auto& reading : resolved_readings) {
                    const std::string component = ToLowerAscii(reading.component);
                    if (component == "left" || component == "right" || component == "case") {
                        tws_components.insert(component);
                    }
                }
                if (tws_components.contains("left") || tws_components.contains("right")) {
                    tws_components.insert("left");
                    tws_components.insert("right");
                }
                accumulator->RemoveTwsBatteryEntriesForAddress(*resolved_address_for_fallback, tws_components);
            }

            for (const auto& battery_reading : resolved_readings) {
                DeviceBatteryInfo entry;
                entry.device_id = device_id;
                entry.device_name = device_name;
                entry.battery_component = battery_reading.component;
                entry.battery_level_percent = battery_reading.percent;
                PopulateBluetoothVisualHintsFromDeviceInfo(device_info, &entry);
                entry.is_cached = resolved_from_persistent_cache;
                entry.is_connected = ble_is_connected;
                accumulator->AddEntry(std::move(entry));
            }

            const auto resolved_address = TryGetBluetoothAddress(ble_device);
            if (resolved_address.has_value() && !resolved_readings.empty() && !resolved_from_persistent_cache) {
                accumulator->MarkAddressWithRealBattery(*resolved_address);
            }
        } catch (const winrt::hresult_error&) {
            // Ignore devices that fail to respond and continue with the next one.
        }
    }
}

}  // namespace battery_monitor

