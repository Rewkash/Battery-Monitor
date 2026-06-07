#include "platform/windows/shared/WindowsTwsCandidateBatteryCollector.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.h>

#include "platform/windows/bluetooth/BleStandardBatteryReader.h"
#include "platform/windows/bluetooth/BleVendorTripletReader.h"
#include "platform/windows/shared/WindowsBatteryEntryUtils.h"
#include "platform/windows/shared/WindowsBatteryProviderSupport.h"
#include "platform/windows/devices/xiaomi/XiaomiBatteryReadings.h"

namespace battery_monitor {

namespace {

using winrt::Windows::Devices::Bluetooth::BluetoothConnectionStatus;

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

std::string ResolveCandidateDeviceId(const EndpointCandidate& candidate) {
    return candidate.endpoint_id.empty() ? ("BluetoothAddress#" + std::to_string(candidate.bluetooth_address))
                                         : candidate.endpoint_id;
}

std::string ResolveCandidateDeviceName(const EndpointCandidate& candidate) {
    return candidate.endpoint_name.empty() ? "Unknown" : candidate.endpoint_name;
}

bool ShouldTraceCandidate(const EndpointCandidate& candidate) {
    const std::string lowered_probe = ToLowerAscii(candidate.endpoint_name + " " + candidate.endpoint_id);
    return lowered_probe.find("redmi") != std::string::npos ||
           lowered_probe.find("buds") != std::string::npos ||
           lowered_probe.find("zmi") != std::string::npos ||
           lowered_probe.find("purpods") != std::string::npos;
}

void AddOfflineCandidateEntry(DeviceBatteryAccumulator* accumulator, const EndpointCandidate& candidate) {
    DeviceBatteryInfo offline_entry;
    offline_entry.device_id = ResolveCandidateDeviceId(candidate);
    offline_entry.device_name = ResolveCandidateDeviceName(candidate);
    offline_entry.battery_component = "main";
    offline_entry.battery_level_percent = std::nullopt;
    PopulateBluetoothVisualHintsFromEndpointCandidate(candidate, &offline_entry);
    offline_entry.is_connected = false;
    accumulator->AddEntry(std::move(offline_entry));
}

void AddUnknownCandidateEntry(DeviceBatteryAccumulator* accumulator,
                              const EndpointCandidate& candidate,
                              bool is_connected) {
    DeviceBatteryInfo unknown_entry;
    unknown_entry.device_id = ResolveCandidateDeviceId(candidate);
    unknown_entry.device_name = ResolveCandidateDeviceName(candidate);
    unknown_entry.battery_component = "main";
    unknown_entry.battery_level_percent = std::nullopt;
    PopulateBluetoothVisualHintsFromEndpointCandidate(candidate, &unknown_entry);
    unknown_entry.is_connected = is_connected;
    accumulator->AddEntry(std::move(unknown_entry));
}

void AddCandidateReadings(DeviceBatteryAccumulator* accumulator,
                          const EndpointCandidate& candidate,
                          const std::vector<BatteryReading>& readings,
                          bool is_cached,
                          bool is_connected,
                          const std::string* device_name_override = nullptr) {
    const std::string device_id = ResolveCandidateDeviceId(candidate);
    const std::string device_name =
        device_name_override != nullptr && !device_name_override->empty() ? *device_name_override
                                                                          : ResolveCandidateDeviceName(candidate);

    for (const auto& battery_reading : readings) {
        DeviceBatteryInfo entry;
        entry.device_id = device_id;
        entry.device_name = device_name;
        entry.battery_component = battery_reading.component;
        entry.battery_level_percent = battery_reading.percent;
        PopulateBluetoothVisualHintsFromEndpointCandidate(candidate, &entry);
        entry.is_cached = is_cached;
        entry.is_connected = is_connected;
        accumulator->AddEntry(std::move(entry));
    }
}

void MergeEndpointCandidates(std::vector<EndpointCandidate>* target_candidates,
                             const std::vector<EndpointCandidate>& discovered_candidates) {
    if (target_candidates == nullptr) {
        return;
    }

    for (const auto& candidate : discovered_candidates) {
        auto existing =
            std::find_if(target_candidates->begin(), target_candidates->end(),
                         [&candidate](const EndpointCandidate& known_candidate) {
                             return known_candidate.bluetooth_address == candidate.bluetooth_address;
                         });
        if (existing == target_candidates->end()) {
            target_candidates->push_back(candidate);
        } else {
            existing->is_connected = existing->is_connected || candidate.is_connected;
        }
    }
}

}  // namespace

void CollectTwsCandidateBatteryEntries(const WindowsTwsCandidateBatteryCollectorContext& context,
                                       const WindowsBatteryQueryReaderContext& query_reader_context,
                                       DeviceBatteryAccumulator* accumulator,
                                       XiaomiClassicBatteryCache* xiaomi_classic_cache) {
    if (accumulator == nullptr || xiaomi_classic_cache == nullptr ||
        context.open_ble_device_by_address == nullptr) {
        return;
    }

    std::vector<EndpointCandidate> tws_candidates;
    const auto fast_connected_entries = ReadConnectedBluetoothDeviceBatteryFast(
        query_reader_context, &tws_candidates, context.target_device_id);

    const bool targeted = !context.target_device_id.empty();
    for (const auto& fast_entry : fast_connected_entries) {
        if (targeted && !DeviceIdMatchesBluetoothTarget(fast_entry.device_id, context.target_device_id)) {
            continue;
        }
        accumulator->AddEntry(fast_entry);
    }
    const bool should_scan_aep = context.force_aep_scan ||
                                 (targeted && tws_candidates.empty()) ||
                                 (!targeted && tws_candidates.size() < 2U);
    if (should_scan_aep) {
        std::vector<EndpointCandidate> aep_tws_candidates;
        const auto endpoint_entries = ReadAssociationEndpointBattery(
            query_reader_context, &aep_tws_candidates, context.target_device_id);
        LogDebug(context.debug_log, "AEP battery entries: " + std::to_string(endpoint_entries.size()));
        LogDebug(context.debug_log, "AEP TWS candidates: " + std::to_string(aep_tws_candidates.size()));

        for (const auto& endpoint_entry : endpoint_entries) {
            accumulator->AddEntry(endpoint_entry);
        }

        MergeEndpointCandidates(&tws_candidates, aep_tws_candidates);
    } else {
        LogDebug(context.debug_log, "AEP scan skipped because fast candidate scan already found targets.");
    }

    for (const auto& candidate : tws_candidates) {
        if (accumulator->AddressesWithRealBattery().contains(candidate.bluetooth_address)) {
            continue;
        }

        if (context.include_disconnected && !candidate.is_connected) {
            AddOfflineCandidateEntry(accumulator, candidate);
            continue;
        }

        const bool target_is_likely_zmi = IsLikelyZmiPurpodsProbe(context.target_device_id);
        const bool candidate_is_likely_zmi = IsLikelyZmiPurpodsProbe(candidate.endpoint_name + " " + candidate.endpoint_id);
        const bool likely_xiaomi_tws =
            (candidate_is_likely_zmi || !target_is_likely_zmi) &&
            context.is_likely_xiaomi_earbuds != nullptr &&
            context.is_likely_xiaomi_earbuds(candidate.endpoint_name, candidate.endpoint_name, candidate.endpoint_id);
        const bool should_try_classic_for_candidate = likely_xiaomi_tws || candidate_is_likely_zmi;
        const auto& addresses_with_real_battery = accumulator->AddressesWithRealBattery();
        if (!targeted && likely_xiaomi_tws && !addresses_with_real_battery.empty() &&
            !addresses_with_real_battery.contains(candidate.bluetooth_address)) {
            LogDebug(context.debug_log,
                     "Xiaomi TWS candidate skipped because another connected TWS battery is available for '" +
                         candidate.endpoint_name + "'");
            continue;
        }
        const bool aggressive_xiaomi_retry =
            candidate.from_connected_scan &&
            !context.include_disconnected &&
            context.should_aggressive_xiaomi_classic_retry != nullptr &&
            context.should_aggressive_xiaomi_classic_retry(
                candidate.endpoint_name, candidate.endpoint_name, candidate.endpoint_id);

        std::vector<BatteryReading> partial_classic_readings;
        bool partial_classic_from_cache = false;
        if (should_try_classic_for_candidate) {
            const auto& classic_result =
                xiaomi_classic_cache->Read(candidate.bluetooth_address,
                                           aggressive_xiaomi_retry,
                                           2U);
            if (!classic_result.readings.empty()) {
                if (candidate_is_likely_zmi && XiaomiResolvedTwsComponentCount(classic_result.readings) >= 1U) {
                    AddCandidateReadings(
                        accumulator,
                        candidate,
                        classic_result.readings,
                        classic_result.from_persistent_cache,
                        candidate.is_connected);
                    LogDebug(context.debug_log,
                             "AEP ZMI classic partial accepted for '" + candidate.endpoint_name + "'");
                    continue;
                }
                if (!HasUsefulXiaomiTwsReadings(classic_result.readings)) {
                    partial_classic_readings = classic_result.readings;
                    partial_classic_from_cache = classic_result.from_persistent_cache;
                    LogDebug(context.debug_log,
                             "AEP Xiaomi classic result is partial for '" + candidate.endpoint_name +
                                 "', continue BLE/vendor fallbacks");
                } else {
                    AddCandidateReadings(
                        accumulator,
                        candidate,
                        classic_result.readings,
                        classic_result.from_persistent_cache,
                        candidate.is_connected);
                    continue;
                }
            }
        }

        const auto maybe_ble_device =
            context.open_ble_device_by_address(candidate.bluetooth_address, std::chrono::milliseconds(1200));
        if (!maybe_ble_device.has_value()) {
            if (context.debug_enabled && ShouldTraceCandidate(candidate)) {
                LogDebug(context.debug_log,
                         "AEP TWS address open failed for '" + candidate.endpoint_name + "' address=" +
                             std::to_string(candidate.bluetooth_address));
            }

            if (!partial_classic_readings.empty()) {
                AddCandidateReadings(
                    accumulator,
                    candidate,
                    partial_classic_readings,
                    partial_classic_from_cache,
                    candidate.is_connected);
                continue;
            }

            if (likely_xiaomi_tws) {
                const auto persisted_result = xiaomi_classic_cache->ReadPersistent(candidate.bluetooth_address, 2U);
                if (!persisted_result.readings.empty() &&
                    HasUsefulXiaomiTwsReadings(persisted_result.readings, 2U)) {
                    AddCandidateReadings(
                        accumulator,
                        candidate,
                        persisted_result.readings,
                        persisted_result.from_persistent_cache,
                        candidate.is_connected);
                    LogDebug(context.debug_log,
                             "AEP fallback: last successful live snapshot used for '" +
                                 ResolveCandidateDeviceName(candidate) + "'");
                    continue;
                }
            }

            AddUnknownCandidateEntry(accumulator, candidate, candidate.is_connected);
            continue;
        }

        const auto ble_device = *maybe_ble_device;
        const bool candidate_ble_connected = ble_device.ConnectionStatus() == BluetoothConnectionStatus::Connected;

        std::vector<BatteryReading> resolved_readings;
        if (!likely_xiaomi_tws) {
            resolved_readings = ReadBleBatteryReadings(ble_device, true);
        } else {
            LogDebug(context.debug_log, "AEP candidate: Xiaomi-family TWS, skip standard battery service read");
        }

        bool resolved_from_persistent_cache = false;
        if (should_try_classic_for_candidate && !HasUsefulXiaomiTwsReadings(resolved_readings)) {
            const auto& classic_result =
                xiaomi_classic_cache->Read(candidate.bluetooth_address,
                                           aggressive_xiaomi_retry,
                                           2U);
            if (!classic_result.readings.empty()) {
                resolved_readings = classic_result.readings;
                resolved_from_persistent_cache = classic_result.from_persistent_cache;
            }
        }

        if (resolved_readings.empty()) {
            const auto fff1_readings = TryReadBleFff1Battery(ble_device, context.debug_enabled, context.debug_log);
            if (!fff1_readings.empty()) {
                resolved_readings = fff1_readings;
                resolved_from_persistent_cache = false;
                LogDebug(context.debug_log,
                         "AEP FFF1 fallback accepted for '" + candidate.endpoint_name + "'");
            }
        }

        if (resolved_readings.size() <= 1U) {
            const auto vendor_triplet =
                TryReadBleVendorTripletBattery(ble_device, context.debug_enabled, context.debug_log);
            if (vendor_triplet.size() >= 2U) {
                resolved_readings = vendor_triplet;
                resolved_from_persistent_cache = false;
                LogDebug(context.debug_log,
                         "AEP vendor triplet fallback accepted for '" + candidate.endpoint_name + "'");
            }
        }

        if (should_try_classic_for_candidate &&
            !HasUsefulXiaomiTwsReadings(resolved_readings) &&
            !partial_classic_readings.empty() &&
            XiaomiReadingsRichnessScore(partial_classic_readings) > XiaomiReadingsRichnessScore(resolved_readings)) {
            resolved_readings = partial_classic_readings;
            resolved_from_persistent_cache = partial_classic_from_cache;
        }

        if (should_try_classic_for_candidate &&
            !HasUsefulXiaomiTwsReadings(resolved_readings)) {
            const auto late_standard_readings = ReadBleBatteryReadings(ble_device, true);
            if (XiaomiReadingsRichnessScore(late_standard_readings) >
                XiaomiReadingsRichnessScore(resolved_readings)) {
                resolved_readings = late_standard_readings;
                resolved_from_persistent_cache = false;
                if (!resolved_readings.empty()) {
                    LogDebug(context.debug_log,
                             "AEP Xiaomi fallback: standard battery service yielded " +
                                 std::to_string(resolved_readings.size()) +
                                 " entries for '" + candidate.endpoint_name + "'");
                }
            }
        }

        if (should_try_classic_for_candidate && !HasUsefulXiaomiTwsReadings(resolved_readings)) {
            const auto persisted_result = xiaomi_classic_cache->ReadPersistent(candidate.bluetooth_address, 2U);
            if (!persisted_result.readings.empty() &&
                XiaomiReadingsRichnessScore(persisted_result.readings) > XiaomiReadingsRichnessScore(resolved_readings)) {
                resolved_readings = persisted_result.readings;
                resolved_from_persistent_cache = persisted_result.from_persistent_cache;
                LogDebug(context.debug_log,
                         "AEP Xiaomi fallback: last successful live snapshot accepted for '" +
                             candidate.endpoint_name + "'");
            }
        }

        if (resolved_readings.empty()) {
            if (context.debug_enabled && ShouldTraceCandidate(candidate)) {
                LogDebug(context.debug_log, "AEP TWS BLE battery not found for '" + candidate.endpoint_name + "'");
            }

            AddUnknownCandidateEntry(accumulator, candidate, candidate_ble_connected);
            continue;
        }

        if (likely_xiaomi_tws &&
            !resolved_from_persistent_cache &&
            HasUsefulXiaomiTwsReadings(resolved_readings, 2U)) {
            xiaomi_classic_cache->Persist(candidate.bluetooth_address, resolved_readings);
        }

        std::string device_name = candidate.endpoint_name;
        if (device_name.empty()) {
            device_name = ToUtf8(ble_device.Name());
        }
        if (device_name.empty()) {
            device_name = "Unknown";
        }

        AddCandidateReadings(
            accumulator, candidate, resolved_readings, resolved_from_persistent_cache, candidate_ble_connected, &device_name);
    }
}

}  // namespace battery_monitor

