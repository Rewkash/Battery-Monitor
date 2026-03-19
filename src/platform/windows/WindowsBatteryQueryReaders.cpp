#include "platform/windows/WindowsBatteryQueryReaders.h"

#include <chrono>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

#include "platform/windows/BluetoothVisualHintProperties.h"
#include "platform/windows/WindowsBluetoothAddressUtils.h"
#include "platform/windows/WindowsBatteryEntryUtils.h"
#include "platform/windows/WindowsDeviceInfoProperties.h"
#include "platform/windows/ZmiVendorBatteryHints.h"

namespace battery_monitor {

namespace {

using winrt::Windows::Devices::Bluetooth::BluetoothConnectionStatus;
using winrt::Windows::Devices::Bluetooth::BluetoothDevice;
using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Devices::Enumeration::DeviceInformationKind;
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

void LogDebug(const WindowsBatteryQueryReaderContext& context, const std::string& message) {
    if (context.debug_log != nullptr) {
        context.debug_log(message);
    }
}

template <typename TOperation>
auto WaitForAsyncResult(TOperation operation, std::chrono::milliseconds timeout)
    -> std::optional<decltype(operation.GetResults())> {
    using TResult = decltype(operation.GetResults());

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

        return std::optional<TResult>(operation.GetResults());
    } catch (const winrt::hresult_error&) {
        return std::nullopt;
    }
}

}  // namespace

std::vector<DeviceBatteryInfo> ReadConnectedBluetoothDeviceBatteryFast(
    const WindowsBatteryQueryReaderContext& context,
    std::vector<EndpointCandidate>* tws_candidates) {
    std::vector<DeviceBatteryInfo> entries;
    std::unordered_set<std::uint64_t> seen_addresses;
    std::unordered_set<std::string> seen_device_ids;
    std::unordered_map<std::uint64_t, std::optional<std::uint8_t>> hfp_phone_cache;
    auto read_phone_hfp_battery_cached = [&](std::uint64_t address) -> std::optional<std::uint8_t> {
        const auto found = hfp_phone_cache.find(address);
        if (found != hfp_phone_cache.end()) {
            return found->second;
        }

        if (context.read_phone_hfp_pnp_hint != nullptr) {
            const auto pnp_hint = context.read_phone_hfp_pnp_hint(address);
            if (pnp_hint.has_value()) {
                hfp_phone_cache.insert_or_assign(address, pnp_hint);
                return pnp_hint;
            }
        }

        const auto read =
            context.read_phone_hfp_fallback != nullptr ? context.read_phone_hfp_fallback(address) : std::nullopt;
        hfp_phone_cache.insert_or_assign(address, read);
        return read;
    };
    auto read_controller_battery_cached =
        [&](const std::string& device_name, const std::string& device_id) -> std::optional<std::uint8_t> {
        if (context.read_controller_battery == nullptr) {
            return std::nullopt;
        }
        return context.read_controller_battery(device_name, device_id);
    };

    auto requested_properties = winrt::single_threaded_vector<winrt::hstring>();
    requested_properties.Append(L"System.ItemNameDisplay");
    requested_properties.Append(L"System.Devices.Aep.DeviceAddress");
    requested_properties.Append(L"System.Devices.Aep.IsConnected");
    requested_properties.Append(L"System.Devices.BatteryLife");
    requested_properties.Append(L"System.Devices.BatteryPlusCharging");
    requested_properties.Append(L"System.Devices.BatteryPlusChargingText");
    AppendBluetoothVisualHintPropertyRequests(requested_properties);
    AppendZmiVendorHintPropertyRequests(requested_properties);

    const auto query_started_at = std::chrono::steady_clock::now();
    const auto maybe_device_infos = WaitForAsyncResult(
        DeviceInformation::FindAllAsync(BluetoothDevice::GetDeviceSelectorFromConnectionStatus(BluetoothConnectionStatus::Connected),
                                        requested_properties,
                                        DeviceInformationKind::Device),
        std::chrono::milliseconds(1800));
    if (!maybe_device_infos.has_value() || !(*maybe_device_infos)) {
        LogDebug(context, "Fast connected-device query failed or timed out.");
        return entries;
    }
    const auto device_infos = *maybe_device_infos;
    if (context.debug_enabled) {
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - query_started_at);
        LogDebug(context, "Fast connected-device query took " + std::to_string(elapsed_ms.count()) + " ms");
    }

    LogDebug(context, "Fast connected-device entries scanned: " + std::to_string(device_infos.Size()));

    for (const auto& device_info : device_infos) {
        std::string device_name = ToUtf8(device_info.Name());
        if (device_name.empty()) {
            TryGetStringProperty(device_info, L"System.ItemNameDisplay", &device_name);
        }
        const std::string device_id = ToUtf8(device_info.Id());
        seen_device_ids.insert(device_id);
        const bool likely_tws =
            context.is_likely_tws_device != nullptr &&
            context.is_likely_tws_device(device_name, device_name, device_id);

        std::optional<std::uint64_t> address;
        std::string address_text;
        if (TryGetStringProperty(device_info, L"System.Devices.Aep.DeviceAddress", &address_text)) {
            address = ParseBluetoothAddress(address_text);
        }
        if (!address.has_value()) {
            address = ParseBluetoothAddressFromDeviceId(device_id);
        }
        if (!address.has_value()) {
            const auto maybe_bt_device =
                WaitForAsyncResult(BluetoothDevice::FromIdAsync(device_info.Id()), std::chrono::milliseconds(700));
            if (maybe_bt_device.has_value() && *maybe_bt_device) {
                try {
                    const auto bt_address = (*maybe_bt_device).BluetoothAddress();
                    if (bt_address > 0xFFFFULL) {
                        address = bt_address;
                    }
                } catch (const winrt::hresult_error&) {
                }
            }
        }

        if (likely_tws && tws_candidates != nullptr && address.has_value() &&
            seen_addresses.insert(*address).second) {
            EndpointCandidate candidate;
            candidate.endpoint_id = device_id;
            candidate.endpoint_name = device_name.empty() ? "Unknown" : device_name;
            candidate.bluetooth_address = *address;
            PopulateBluetoothVisualHintsFromDeviceInfo(device_info, &candidate);
            candidate.from_connected_scan = true;
            candidate.is_connected = true;
            tws_candidates->push_back(std::move(candidate));
        }

        if (context.is_likely_zmi_purpods != nullptr &&
            context.is_likely_zmi_purpods(device_name, device_name, device_id) &&
            TryAppendZmiVendorBatteryEntries(&entries,
                                             device_info,
                                             device_id,
                                             device_name,
                                             address,
                                             true,
                                             context.debug_enabled,
                                             context.debug_log,
                                             "Fast connected fallback")) {
            continue;
        }

        auto battery_percent = ReadBatteryPercentFromEndpointProperties(device_info);
        if (!battery_percent.has_value() &&
            address.has_value() &&
            context.is_likely_phone_device != nullptr &&
            context.is_likely_phone_device(device_name, device_name, device_id)) {
            battery_percent = read_phone_hfp_battery_cached(*address);
        }
        if (!battery_percent.has_value() &&
            context.is_likely_game_controller_device != nullptr &&
            context.is_likely_game_controller_device(device_name, device_name, device_id)) {
            battery_percent = read_controller_battery_cached(device_name, device_id);
        }
        AppendSingleBatteryEntry(&entries, device_info, device_id, device_name, battery_percent, true);
    }

    if (tws_candidates != nullptr && tws_candidates->size() < 2U) {
        try {
            const auto maybe_paired_infos = WaitForAsyncResult(
                DeviceInformation::FindAllAsync(BluetoothDevice::GetDeviceSelectorFromPairingState(true),
                                                requested_properties,
                                                DeviceInformationKind::Device),
                std::chrono::milliseconds(2200));
            if (!maybe_paired_infos.has_value() || !(*maybe_paired_infos)) {
                LogDebug(context, "Paired-device query failed or timed out.");
                return entries;
            }
            const auto paired_infos = *maybe_paired_infos;
            for (const auto& device_info : paired_infos) {
                std::string device_name = ToUtf8(device_info.Name());
                if (device_name.empty()) {
                    TryGetStringProperty(device_info, L"System.ItemNameDisplay", &device_name);
                }
                const std::string device_id = ToUtf8(device_info.Id());
                if (!seen_device_ids.insert(device_id).second) {
                    continue;
                }

                const bool likely_tws =
                    context.is_likely_tws_device != nullptr &&
                    context.is_likely_tws_device(device_name, device_name, device_id);
                const bool likely_zmi =
                    context.is_likely_zmi_purpods != nullptr &&
                    context.is_likely_zmi_purpods(device_name, device_name, device_id);
                if (!likely_tws && !likely_zmi) {
                    continue;
                }

                bool is_connected = false;
                TryGetBooleanProperty(device_info, L"System.Devices.Aep.IsConnected", &is_connected);

                std::optional<std::uint64_t> address;
                std::string address_text;
                if (TryGetStringProperty(device_info, L"System.Devices.Aep.DeviceAddress", &address_text)) {
                    address = ParseBluetoothAddress(address_text);
                }
                if (!address.has_value()) {
                    address = ParseBluetoothAddressFromDeviceId(device_id);
                }

                if (likely_tws && address.has_value() && seen_addresses.insert(*address).second) {
                    EndpointCandidate candidate;
                    candidate.endpoint_id = device_id;
                    candidate.endpoint_name = device_name.empty() ? "Unknown" : device_name;
                    candidate.bluetooth_address = *address;
                    PopulateBluetoothVisualHintsFromDeviceInfo(device_info, &candidate);
                    candidate.from_connected_scan = false;
                    candidate.is_connected = is_connected;
                    tws_candidates->push_back(std::move(candidate));
                }

                if (likely_zmi &&
                    TryAppendZmiVendorBatteryEntries(&entries,
                                                     device_info,
                                                     device_id,
                                                     device_name,
                                                     address,
                                                     is_connected,
                                                     context.debug_enabled,
                                                     context.debug_log,
                                                     nullptr)) {
                    continue;
                }

                auto battery_percent = ReadBatteryPercentFromEndpointProperties(device_info);
                AppendSingleBatteryEntry(&entries, device_info, device_id, device_name, battery_percent, is_connected);
            }
        } catch (const winrt::hresult_error&) {
            // Paired device fallback is best-effort.
        }
    }

    LogDebug(context, "Fast connected battery entries: " + std::to_string(entries.size()));
    if (tws_candidates != nullptr) {
        LogDebug(context, "Fast TWS candidates: " + std::to_string(tws_candidates->size()));
    }
    return entries;
}

std::vector<DeviceBatteryInfo> ReadAssociationEndpointBattery(const WindowsBatteryQueryReaderContext& context,
                                                             std::vector<EndpointCandidate>* tws_candidates) {
    std::vector<DeviceBatteryInfo> endpoint_entries;
    std::unordered_set<std::string> seen_tws_addresses;

    auto requested_properties = winrt::single_threaded_vector<winrt::hstring>();
    requested_properties.Append(L"System.Devices.Aep.IsConnected");
    requested_properties.Append(L"System.Devices.Aep.ProtocolId");
    requested_properties.Append(L"System.Devices.Aep.DeviceAddress");
    requested_properties.Append(L"System.Devices.BatteryLife");
    requested_properties.Append(L"System.Devices.BatteryPlusCharging");
    requested_properties.Append(L"System.Devices.BatteryPlusChargingText");
    requested_properties.Append(L"System.ItemNameDisplay");
    AppendBluetoothVisualHintPropertyRequests(requested_properties);
    AppendZmiVendorHintPropertyRequests(requested_properties);

    constexpr auto kEndpointSelector = LR"((System.Devices.Aep.IsPresent:=System.StructuredQueryType.Boolean#True)
AND ((System.Devices.Aep.ProtocolId:="{E0CBF06C-CD8B-4647-BB8A-263B43F0F974}")
OR (System.Devices.Aep.ProtocolId:="{BB7BB05E-5972-42B5-94FC-76EAA7084D49}")))";

    const auto query_started_at = std::chrono::steady_clock::now();
    const auto endpoint_infos_operation =
        DeviceInformation::FindAllAsync(kEndpointSelector, requested_properties, DeviceInformationKind::AssociationEndpoint);
    const auto endpoint_infos_result =
        WaitForAsyncResult(endpoint_infos_operation, std::chrono::milliseconds(3600));
    if (context.debug_enabled) {
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - query_started_at);
        LogDebug(context, "AEP query took " + std::to_string(elapsed_ms.count()) + " ms");
    }

    if (!endpoint_infos_result.has_value()) {
        LogDebug(context, "AEP query timed out, skipping slow fallback.");
        return endpoint_entries;
    }

    const auto endpoint_infos = *endpoint_infos_result;
    LogDebug(context, "AEP entries scanned: " + std::to_string(endpoint_infos.Size()));

    for (const auto& endpoint_info : endpoint_infos) {
        if (!IsLikelyBluetoothEndpoint(endpoint_info)) {
            continue;
        }

        std::string endpoint_name = ToUtf8(endpoint_info.Name());
        if (endpoint_name.empty()) {
            TryGetStringProperty(endpoint_info, L"System.ItemNameDisplay", &endpoint_name);
        }
        if (endpoint_name.empty()) {
            endpoint_name = "Unknown";
        }
        const std::string endpoint_id = ToUtf8(endpoint_info.Id());
        bool endpoint_is_connected = false;
        TryGetBooleanProperty(endpoint_info, L"System.Devices.Aep.IsConnected", &endpoint_is_connected);
        const std::string endpoint_name_lower = ToLowerAscii(endpoint_name);
        const bool interesting = endpoint_name_lower.find("redmi") != std::string::npos ||
                                 endpoint_name_lower.find("buds") != std::string::npos ||
                                 endpoint_name_lower.find("zmi") != std::string::npos ||
                                 endpoint_name_lower.find("purpods") != std::string::npos;
        if (interesting) {
            LogDebug(context, "AEP candidate: name='" + endpoint_name + "' id='" + endpoint_id + "'");
        }

        if (tws_candidates != nullptr &&
            context.looks_like_tws_device_by_name != nullptr &&
            context.looks_like_tws_device_by_name(endpoint_name)) {
            std::optional<std::uint64_t> address;
            std::string address_text;
            if (TryGetStringProperty(endpoint_info, L"System.Devices.Aep.DeviceAddress", &address_text)) {
                address = ParseBluetoothAddress(address_text);
                if (interesting) {
                    LogDebug(context, "AEP DeviceAddress raw text: '" + address_text + "'");
                }
            }

            if (!address.has_value()) {
                std::uint64_t numeric_address = 0;
                if (TryGetUInt64Property(endpoint_info, L"System.Devices.Aep.DeviceAddress", &numeric_address) &&
                    numeric_address > 0xFFFFULL) {
                    address = numeric_address;
                }
            }

            if (!address.has_value()) {
                address = ParseBluetoothAddressFromDeviceId(endpoint_id);
                if (interesting && address.has_value()) {
                    LogDebug(context, "AEP address parsed from device id");
                }
            }

            if (address.has_value()) {
                const std::string address_key = std::to_string(*address);
                if (seen_tws_addresses.insert(address_key).second) {
                    EndpointCandidate candidate;
                    candidate.endpoint_id = endpoint_id;
                    candidate.endpoint_name = endpoint_name;
                    candidate.bluetooth_address = *address;
                    PopulateBluetoothVisualHintsFromDeviceInfo(endpoint_info, &candidate);
                    candidate.from_connected_scan = false;
                    candidate.is_connected = endpoint_is_connected;
                    tws_candidates->push_back(std::move(candidate));
                    if (interesting) {
                        LogDebug(context, "AEP tws candidate address: " + address_key);
                    }
                }
            } else if (interesting) {
                LogDebug(context, "AEP tws candidate has no parseable DeviceAddress");
            }
        }

        if (context.is_likely_zmi_purpods != nullptr &&
            context.is_likely_zmi_purpods(endpoint_name, endpoint_name, endpoint_id) &&
            TryAppendZmiVendorBatteryEntries(&endpoint_entries,
                                             endpoint_info,
                                             endpoint_id,
                                             endpoint_name,
                                             ParseBluetoothAddressFromDeviceId(endpoint_id),
                                             endpoint_is_connected,
                                             context.debug_enabled,
                                             context.debug_log,
                                             "AEP fallback")) {
            continue;
        }

        auto battery_percent = ReadBatteryPercentFromEndpointProperties(endpoint_info);
        if (!battery_percent.has_value()) {
            if (interesting) {
                LogDebug(context, "AEP battery props not found for '" + endpoint_name + "'");
            }
            continue;
        }

        AppendSingleBatteryEntry(&endpoint_entries, endpoint_info, endpoint_id, endpoint_name, *battery_percent,
                                 endpoint_is_connected);
        if (interesting) {
            LogDebug(context, "AEP battery found for '" + endpoint_name + "': " + std::to_string(*battery_percent));
        }
    }

    return endpoint_entries;
}

std::vector<DeviceBatteryInfo> ReadGenericDeviceBattery(const WindowsBatteryQueryReaderContext& context) {
    std::vector<DeviceBatteryInfo> entries;

    auto requested_properties = winrt::single_threaded_vector<winrt::hstring>();
    requested_properties.Append(L"System.Devices.Aep.IsConnected");
    requested_properties.Append(L"System.Devices.BatteryLife");
    requested_properties.Append(L"System.Devices.BatteryPlusCharging");
    requested_properties.Append(L"System.Devices.BatteryPlusChargingText");
    requested_properties.Append(L"System.ItemNameDisplay");
    AppendBluetoothVisualHintPropertyRequests(requested_properties);
    AppendZmiVendorHintPropertyRequests(requested_properties);

    constexpr auto kDeviceSelector = LR"(System.Devices.IsPresent:=System.StructuredQueryType.Boolean#True)";

    const auto maybe_devices = WaitForAsyncResult(
        DeviceInformation::FindAllAsync(kDeviceSelector, requested_properties, DeviceInformationKind::Device),
        std::chrono::milliseconds(2200));
    if (!maybe_devices.has_value() || !(*maybe_devices)) {
        LogDebug(context, "Generic Device query failed or timed out.");
        return entries;
    }
    const auto devices = *maybe_devices;
    LogDebug(context, "Generic Device entries scanned: " + std::to_string(devices.Size()));

    for (const auto& device : devices) {
        if (!IsLikelyBluetoothDeviceInfo(device)) {
            continue;
        }

        const std::string device_name_probe = ToUtf8(device.Name());
        const std::string device_id = ToUtf8(device.Id());
        const std::string lowered_probe = ToLowerAscii(device_name_probe + " " + device_id);
        const bool interesting = lowered_probe.find("redmi") != std::string::npos ||
                                 lowered_probe.find("buds") != std::string::npos ||
                                 lowered_probe.find("zmi") != std::string::npos ||
                                 lowered_probe.find("purpods") != std::string::npos;

        std::string name = ToUtf8(device.Name());
        if (name.empty()) {
            TryGetStringProperty(device, L"System.ItemNameDisplay", &name);
        }
        if (name.empty()) {
            name = "Unknown";
        }

        bool is_connected = false;
        TryGetBooleanProperty(device, L"System.Devices.Aep.IsConnected", &is_connected);

        if (context.is_likely_zmi_purpods != nullptr &&
            context.is_likely_zmi_purpods(device_name_probe, device_name_probe, device_id) &&
            TryAppendZmiVendorBatteryEntries(&entries,
                                             device,
                                             device_id,
                                             name,
                                             ParseBluetoothAddressFromDeviceId(device_id),
                                             is_connected,
                                             context.debug_enabled,
                                             context.debug_log,
                                             "Generic fallback")) {
            continue;
        }

        auto battery = ReadBatteryPercentFromEndpointProperties(device);
        if (!battery.has_value()) {
            if (interesting) {
                LogDebug(context, "Generic battery props not found for name='" + device_name_probe + "' id='" + device_id + "'");
            }
            continue;
        }

        AppendSingleBatteryEntry(&entries, device, device_id, name, *battery, is_connected);
        if (interesting) {
            LogDebug(context, "Generic battery found: name='" + name + "' battery=" + std::to_string(*battery));
        }
    }

    return entries;
}

}  // namespace battery_monitor
