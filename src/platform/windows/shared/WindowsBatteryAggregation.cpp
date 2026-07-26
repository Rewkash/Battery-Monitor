#include "platform/windows/shared/WindowsBatteryAggregation.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

#include "platform/windows/shared/BatteryComponentNaming.h"
#include "platform/windows/devices/phone/BluetoothPnpHints.h"
#include "platform/windows/shared/BluetoothVisualHintProperties.h"
#include "platform/windows/shared/WindowsBluetoothAddressUtils.h"
#include "platform/windows/shared/WindowsDeviceInfoProperties.h"
#include "platform/windows/devices/xiaomi/XiaomiModeCache.h"

namespace battery_monitor {

namespace {

using winrt::Windows::Devices::Bluetooth::BluetoothDevice;
using winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Devices::Enumeration::DeviceInformationKind;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncOperation;

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

std::string BatteryValueTag(const std::optional<std::uint8_t>& battery_level_percent) {
    if (!battery_level_percent.has_value()) {
        return "na";
    }
    return std::to_string(*battery_level_percent);
}

std::string MakeEntryKey(const DeviceBatteryInfo& entry) {
    return entry.device_id + "|" + entry.battery_component + "|" + BatteryValueTag(entry.battery_level_percent) +
           "|" + (entry.is_cached ? "cached" : "live");
}

template <typename TResult>
std::optional<TResult> WaitForAsyncResult(IAsyncOperation<TResult> operation, std::chrono::milliseconds timeout) {
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

void DeviceBatteryAccumulator::AddEntry(DeviceBatteryInfo entry) {
    const auto parsed_address = ParseBluetoothAddressFromDeviceId(entry.device_id);
    if (parsed_address.has_value()) {
        if (!entry.device_mode.has_value()) {
            entry.device_mode = TryGetXiaomiModeCacheEntry(*parsed_address);
        }
        if (!entry.device_submode.has_value()) {
            entry.device_submode = TryGetXiaomiSubmodeCacheEntry(*parsed_address);
        }
    }

    const std::string dedupe_key = MakeEntryKey(entry);
    const auto known = known_entries_.find(dedupe_key);
    if (known != known_entries_.end()) {
        auto& existing = entries_[known->second];
        existing.is_connected = existing.is_connected || entry.is_connected;
        if (!existing.device_mode.has_value() && entry.device_mode.has_value()) {
            existing.device_mode = entry.device_mode;
        }
        if (!existing.device_submode.has_value() && entry.device_submode.has_value()) {
            existing.device_submode = entry.device_submode;
        }
        if (!existing.bluetooth_le_appearance.has_value() && entry.bluetooth_le_appearance.has_value()) {
            existing.bluetooth_le_appearance = entry.bluetooth_le_appearance;
        }
        if (!existing.bluetooth_cod_major.has_value() && entry.bluetooth_cod_major.has_value()) {
            existing.bluetooth_cod_major = entry.bluetooth_cod_major;
        }
        if (!existing.bluetooth_cod_minor.has_value() && entry.bluetooth_cod_minor.has_value()) {
            existing.bluetooth_cod_minor = entry.bluetooth_cod_minor;
        }
        AppendUniqueStrings(&existing.device_categories, entry.device_categories);
        return;
    }

    if (entry.battery_level_percent.has_value() && !entry.is_cached && parsed_address.has_value()) {
        addresses_with_real_battery_.insert(*parsed_address);
    }

    known_entries_.emplace(dedupe_key, entries_.size());
    entries_.push_back(std::move(entry));
}

void DeviceBatteryAccumulator::RemoveTwsBatteryEntriesForAddress(std::uint64_t address) {
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                       [address](const DeviceBatteryInfo& entry) {
                           const auto parsed_address = ParseBluetoothAddressFromDeviceId(entry.device_id);
                           if (!parsed_address.has_value() || *parsed_address != address) {
                               return false;
                           }
                           const std::string component = ToLowerAscii(entry.battery_component);
                           return component == "left" || component == "right" || component == "case";
                       }),
        entries_.end());

    known_entries_.clear();
    addresses_with_real_battery_.erase(address);
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const auto& entry = entries_[i];
        known_entries_.emplace(MakeEntryKey(entry), i);
        const auto parsed_address = ParseBluetoothAddressFromDeviceId(entry.device_id);
        if (entry.battery_level_percent.has_value() && !entry.is_cached && parsed_address.has_value()) {
            addresses_with_real_battery_.insert(*parsed_address);
        }
    }
}

void DeviceBatteryAccumulator::MarkAddressWithRealBattery(std::uint64_t address) {
    if (address > 0xFFFFULL) {
        addresses_with_real_battery_.insert(address);
    }
}

bool DeviceBatteryAccumulator::Empty() const {
    return entries_.empty();
}

const std::vector<DeviceBatteryInfo>& DeviceBatteryAccumulator::Entries() const {
    return entries_;
}

const std::unordered_set<std::uint64_t>& DeviceBatteryAccumulator::AddressesWithRealBattery() const {
    return addresses_with_real_battery_;
}

std::vector<DeviceBatteryInfo> DeviceBatteryAccumulator::TakeEntries() {
    return std::move(entries_);
}

DisconnectedPairedCollection CollectDisconnectedPairedBluetoothEntries(bool debug_enabled, XiaomiDebugLogFn debug_log) {
    DisconnectedPairedCollection result;
    (void)debug_enabled;

    auto requested_properties = winrt::single_threaded_vector<winrt::hstring>();
    requested_properties.Append(L"System.ItemNameDisplay");
    requested_properties.Append(L"System.Devices.Aep.DeviceAddress");
    requested_properties.Append(L"System.Devices.Aep.IsConnected");
    AppendBluetoothVisualHintPropertyRequests(requested_properties);

    std::unordered_set<std::string> processed_paired_ids;
    auto collect_paired = [&](const winrt::hstring& selector) {
        const auto maybe_paired_infos = WaitForAsyncResult(
            DeviceInformation::FindAllAsync(selector, requested_properties, DeviceInformationKind::Device),
            std::chrono::milliseconds(2200));
        if (!maybe_paired_infos.has_value() || !(*maybe_paired_infos)) {
            return;
        }
        result.snapshot.loaded = true;
        const auto paired_infos = *maybe_paired_infos;
        for (const auto& device_info : paired_infos) {
            if (!IsLikelyBluetoothDeviceInfo(device_info)) {
                continue;
            }

            const std::string device_id = ToUtf8(device_info.Id());
            if (device_id.empty() || !processed_paired_ids.insert(device_id).second) {
                continue;
            }

            std::string device_name = ToUtf8(device_info.Name());
            if (device_name.empty()) {
                TryGetStringProperty(device_info, L"System.ItemNameDisplay", &device_name);
            }
            if (device_name.empty()) {
                device_name = "Unknown";
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
            result.snapshot.device_ids.insert(device_id);
            if (address.has_value()) {
                result.snapshot.addresses.insert(*address);
            }
            if (is_connected) {
                continue;
            }

            DeviceBatteryInfo entry;
            entry.device_id = device_id;
            entry.device_name = device_name;
            entry.battery_component = NormalizeBatteryComponentHint(device_name);
            if (entry.battery_component.empty()) {
                entry.battery_component = "main";
            }
            entry.is_connected = false;
            result.offline_entries.push_back(std::move(entry));
        }
    };

    try {
        collect_paired(BluetoothDevice::GetDeviceSelectorFromPairingState(true));
        collect_paired(BluetoothLEDevice::GetDeviceSelectorFromPairingState(true));
    } catch (const winrt::hresult_error&) {
        LogDebug(debug_log, "Paired fallback for disconnected devices failed.");
    }

    return result;
}

void ApplyPnpVisualHints(std::vector<DeviceBatteryInfo>* entries) {
    if (entries == nullptr) {
        return;
    }

    std::unordered_map<std::uint64_t, std::optional<PnpBluetoothVisualHints>> pnp_visual_hint_cache;
    for (auto& entry : *entries) {
        const bool needs_pnp_visual_hints =
            !entry.bluetooth_cod_major.has_value() && !entry.bluetooth_cod_minor.has_value() &&
            entry.device_categories.empty();
        if (!needs_pnp_visual_hints) {
            continue;
        }

        const auto parsed_address = ParseBluetoothAddressFromDeviceId(entry.device_id);
        if (!parsed_address.has_value()) {
            continue;
        }

        auto cache_it = pnp_visual_hint_cache.find(*parsed_address);
        if (cache_it == pnp_visual_hint_cache.end()) {
            cache_it = pnp_visual_hint_cache.emplace(
                *parsed_address, ReadBluetoothVisualHintsFromPnpAddress(*parsed_address)).first;
        }
        if (!cache_it->second.has_value()) {
            continue;
        }

        const auto& hints = *cache_it->second;
        if (!entry.bluetooth_cod_major.has_value() && hints.bluetooth_cod_major.has_value()) {
            entry.bluetooth_cod_major = hints.bluetooth_cod_major;
        }
        if (!entry.bluetooth_cod_minor.has_value() && hints.bluetooth_cod_minor.has_value()) {
            entry.bluetooth_cod_minor = hints.bluetooth_cod_minor;
        }
        AppendUniqueStrings(&entry.device_categories, hints.device_categories);
    }
}

std::vector<DeviceBatteryInfo> FinalizeCollectedEntries(std::vector<DeviceBatteryInfo> entries,
                                                        bool include_disconnected,
                                                        const PairedBluetoothSnapshot* paired_snapshot) {
    std::unordered_set<std::string> devices_with_real_battery;
    std::unordered_set<std::string> devices_with_live_battery;
    std::unordered_set<std::uint64_t> addresses_with_live_battery;
    std::unordered_set<std::uint64_t> addresses_with_any_real_battery;
    std::unordered_set<std::string> devices_with_tws_components;
    std::unordered_set<std::uint64_t> addresses_with_tws_components;
    for (const auto& entry : entries) {
        if (entry.battery_level_percent.has_value()) {
            devices_with_real_battery.insert(entry.device_id);
            const auto parsed_address = ParseBluetoothAddressFromDeviceId(entry.device_id);
            if (parsed_address.has_value()) {
                addresses_with_any_real_battery.insert(*parsed_address);
            }
            if (!entry.is_cached) {
                devices_with_live_battery.insert(entry.device_id);
                if (parsed_address.has_value()) {
                    addresses_with_live_battery.insert(*parsed_address);
                }
            }
        }

        if (entry.battery_level_percent.has_value()) {
            const std::string component = ToLowerAscii(entry.battery_component);
            if (component == "left" || component == "right" || component == "case") {
                const auto parsed_address = ParseBluetoothAddressFromDeviceId(entry.device_id);
                if (parsed_address.has_value()) {
                    addresses_with_tws_components.insert(*parsed_address);
                } else {
                    devices_with_tws_components.insert(entry.device_id);
                }
            }
        }
    }

    std::vector<DeviceBatteryInfo> filtered_entries;
    filtered_entries.reserve(entries.size());
    std::unordered_map<std::string, std::size_t> final_dedup;
    for (auto& entry : entries) {
        const auto parsed_address = ParseBluetoothAddressFromDeviceId(entry.device_id);
        const std::string normalized_component = ToLowerAscii(entry.battery_component);

        if (normalized_component == "main" &&
            (devices_with_tws_components.contains(entry.device_id) ||
             (parsed_address.has_value() && addresses_with_tws_components.contains(*parsed_address)))) {
            continue;
        }

        if (!entry.battery_level_percent.has_value() &&
            (devices_with_real_battery.contains(entry.device_id) ||
             (parsed_address.has_value() && addresses_with_any_real_battery.contains(*parsed_address)))) {
            continue;
        }
        if (entry.is_cached &&
            (devices_with_live_battery.contains(entry.device_id) ||
             (parsed_address.has_value() && addresses_with_live_battery.contains(*parsed_address)))) {
            continue;
        }

        std::string key;
        if (!entry.battery_level_percent.has_value()) {
            if (parsed_address.has_value()) {
                key = "unknown|" + std::to_string(*parsed_address) + "|" + entry.battery_component;
            } else {
                key = "unknown|" + entry.device_id + "|" + entry.battery_component;
            }
        } else {
            if (parsed_address.has_value()) {
                key = "resolved|" + std::to_string(*parsed_address) + "|" + entry.battery_component +
                      "|" + (entry.is_cached ? "cached" : "live");
            } else {
                key = "resolved|" + entry.device_id + "|" + entry.battery_component +
                      "|" + (entry.is_cached ? "cached" : "live");
            }
        }
        const auto dedup_it = final_dedup.find(key);
        if (dedup_it != final_dedup.end()) {
            auto& existing = filtered_entries[dedup_it->second];
            entry.is_connected = existing.is_connected || entry.is_connected;
            if (!entry.device_mode.has_value()) {
                entry.device_mode = existing.device_mode;
            }
            if (!entry.device_submode.has_value()) {
                entry.device_submode = existing.device_submode;
            }
            if (!entry.bluetooth_le_appearance.has_value()) {
                entry.bluetooth_le_appearance = existing.bluetooth_le_appearance;
            }
            if (!entry.bluetooth_cod_major.has_value()) {
                entry.bluetooth_cod_major = existing.bluetooth_cod_major;
            }
            if (!entry.bluetooth_cod_minor.has_value()) {
                entry.bluetooth_cod_minor = existing.bluetooth_cod_minor;
            }
            AppendUniqueStrings(&entry.device_categories, existing.device_categories);
            existing = std::move(entry);
            continue;
        }

        final_dedup.emplace(key, filtered_entries.size());
        filtered_entries.push_back(std::move(entry));
    }

    if (include_disconnected && paired_snapshot != nullptr && paired_snapshot->loaded) {
        filtered_entries.erase(
            std::remove_if(filtered_entries.begin(), filtered_entries.end(),
                           [&](const DeviceBatteryInfo& entry) {
                               if (paired_snapshot->device_ids.contains(entry.device_id)) {
                                   return false;
                               }
                               const auto parsed_address = ParseBluetoothAddressFromDeviceId(entry.device_id);
                               return !parsed_address.has_value() || !paired_snapshot->addresses.contains(*parsed_address);
                           }),
            filtered_entries.end());
    }

    if (!include_disconnected) {
        filtered_entries.erase(
            std::remove_if(filtered_entries.begin(), filtered_entries.end(),
                           [](const DeviceBatteryInfo& entry) {
                               return !entry.is_connected;
                           }),
            filtered_entries.end());
    }

    return filtered_entries;
}

}  // namespace battery_monitor

