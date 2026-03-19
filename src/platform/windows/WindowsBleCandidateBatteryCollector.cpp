#include "platform/windows/WindowsBleCandidateBatteryCollector.h"

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include "platform/windows/BleCandidateEnumeration.h"
#include "platform/windows/BleStandardBatteryReader.h"
#include "platform/windows/BleVendorTripletReader.h"
#include "platform/windows/WindowsBatteryEntryUtils.h"
#include "platform/windows/WindowsBluetoothAddressUtils.h"
#include "platform/windows/WindowsDeviceInfoProperties.h"
#include "platform/windows/XiaomiBatteryReadings.h"

namespace battery_monitor {

namespace {

using winrt::Windows::Devices::Bluetooth::BluetoothConnectionStatus;
using winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
using winrt::Windows::Foundation::AsyncStatus;

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

void LogDebug(XiaomiDebugLogFn debug_log, const std::string& message) {
    if (debug_log != nullptr) {
        debug_log(message);
    }
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
                                       XiaomiClassicBatteryCache* xiaomi_classic_cache,
                                       XiaomiAdvertisementBatteryCache* xiaomi_advertisement_cache) {
    if (accumulator == nullptr || xiaomi_classic_cache == nullptr || xiaomi_advertisement_cache == nullptr) {
        return;
    }

    const auto device_infos = EnumerateBleCandidateDevices(context.debug_enabled, context.debug_log);
    LogDebug(context.debug_log, "BLE candidates from selectors: " + std::to_string(device_infos.size()));

    for (const auto& device_info : device_infos) {
        try {
            const std::string candidate_id = ToUtf8(device_info.Id());
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
            const bool likely_xiaomi_tws =
                likely_tws &&
                context.is_likely_xiaomi_earbuds != nullptr &&
                context.is_likely_xiaomi_earbuds(device_name, ble_name, device_id);
            const bool likely_zmi_family =
                context.is_likely_zmi_purpods != nullptr &&
                context.is_likely_zmi_purpods(device_name, ble_name, device_id);
            const bool aggressive_xiaomi_retry =
                context.should_aggressive_xiaomi_classic_retry != nullptr &&
                context.should_aggressive_xiaomi_classic_retry(device_name, ble_name, device_id);
            if (context.debug_enabled) {
                const std::string lowered_probe = ToLowerAscii(device_name + " " + ble_name + " " + device_id);
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

            std::vector<BatteryReading> battery_readings;
            if (!likely_xiaomi_tws || likely_zmi_family) {
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
                                                   aggressive_xiaomi_retry,
                                                   likely_zmi_family,
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

            if (likely_tws && resolved_readings.size() <= 1U) {
                const auto vendor_triplet = TryReadBleVendorTripletBattery(ble_device, context.debug_enabled, context.debug_log);
                if (vendor_triplet.size() >= 2U) {
                    resolved_readings = vendor_triplet;
                    resolved_from_persistent_cache = false;
                    LogDebug(context.debug_log, "BLE vendor triplet fallback accepted for '" + device_name + "'");
                }
            }

            if (likely_xiaomi_tws &&
                !HasUsefulXiaomiTwsReadings(resolved_readings) &&
                resolved_address_for_fallback.has_value()) {
                const auto advertisement_readings =
                    xiaomi_advertisement_cache->Read(*resolved_address_for_fallback, device_name, likely_zmi_family);
                if (advertisement_readings.size() >= 2U) {
                    resolved_readings = advertisement_readings;
                    resolved_from_persistent_cache = false;
                    LogDebug(context.debug_log, "BLE advertisement fallback accepted for '" + device_name + "'");
                }
            }

            if (likely_xiaomi_tws &&
                !likely_zmi_family &&
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

            if (likely_xiaomi_tws &&
                !HasUsefulXiaomiTwsReadings(resolved_readings) &&
                resolved_address_for_fallback.has_value()) {
                const auto persisted_result = xiaomi_classic_cache->ReadPersistent(*resolved_address_for_fallback, 2U);
                if (!persisted_result.readings.empty() &&
                    XiaomiReadingsRichnessScore(persisted_result.readings) > XiaomiReadingsRichnessScore(resolved_readings)) {
                    resolved_readings = persisted_result.readings;
                    resolved_from_persistent_cache = persisted_result.from_persistent_cache;
                    LogDebug(context.debug_log,
                             "BLE Xiaomi fallback: persisted TWS snapshot accepted for '" + device_name + "'");
                }
            }

            const bool ble_is_connected = ble_device.ConnectionStatus() == BluetoothConnectionStatus::Connected;
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
                    const std::string lowered_probe = ToLowerAscii(device_name + " " + ble_name + " " + device_id);
                    if (lowered_probe.find("redmi") != std::string::npos ||
                        lowered_probe.find("buds") != std::string::npos ||
                        lowered_probe.find("zmi") != std::string::npos ||
                        lowered_probe.find("purpods") != std::string::npos) {
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
                continue;
            }

            if (likely_xiaomi_tws &&
                resolved_address_for_fallback.has_value() &&
                !resolved_from_persistent_cache &&
                HasUsefulXiaomiTwsReadings(resolved_readings, 2U)) {
                xiaomi_classic_cache->Persist(*resolved_address_for_fallback, resolved_readings);
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
