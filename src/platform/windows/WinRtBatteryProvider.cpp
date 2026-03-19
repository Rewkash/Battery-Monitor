#include "platform/windows/WinRtBatteryProvider.h"

#include <algorithm>
#include <exception>
#include <iostream>
#include <string>
#include <utility>

#include <winrt/base.h>

#include "core/NoiseControlVocabulary.h"
#include "platform/windows/shared/WindowsBatteryAggregation.h"
#include "platform/windows/shared/WindowsBatteryProviderSupport.h"
#include "platform/windows/shared/WindowsBatteryQueryReaders.h"
#include "platform/windows/shared/WindowsBleCandidateBatteryCollector.h"
#include "platform/windows/shared/WindowsBluetoothAddressUtils.h"
#include "platform/windows/shared/WindowsTwsCandidateBatteryCollector.h"
#include "platform/windows/devices/xiaomi/XiaomiBatteryCaches.h"
#include "platform/windows/devices/xiaomi/XiaomiControlActions.h"

namespace battery_monitor {

std::vector<DeviceBatteryInfo> WinRtBatteryProvider::GetDevicesBattery(const BatteryQueryOptions& options) {
    EnsureWindowsBatteryProviderApartmentInitialized();
    const auto& runtime_options = GetWindowsBatteryProviderRuntimeOptions();

    try {
        const WindowsBatteryQueryReaderContext query_reader_context = MakeWindowsBatteryQueryReaderContext();
        DeviceBatteryAccumulator device_accumulator;
        auto try_add_entry = [&](DeviceBatteryInfo entry) {
            device_accumulator.AddEntry(std::move(entry));
        };

        XiaomiClassicBatteryCache xiaomi_classic_cache(
            runtime_options.persistent_xiaomi_cache_write_enabled,
            runtime_options.persistent_xiaomi_cache_read_enabled,
            runtime_options.xiaomi_cache_file_path,
            runtime_options.xiaomi_cache_ttl_minutes,
            runtime_options.debug_enabled,
            &WindowsBatteryProviderDebugLog);

        CollectBleCandidateBatteryEntries(
            MakeWindowsBleCandidateBatteryCollectorContext(),
            &device_accumulator,
            &xiaomi_classic_cache);

        try {
            CollectTwsCandidateBatteryEntries(
                MakeWindowsTwsCandidateBatteryCollectorContext(options.include_disconnected),
                query_reader_context,
                &device_accumulator,
                &xiaomi_classic_cache);
        } catch (const winrt::hresult_error&) {
            // Endpoint properties and endpoint BLE mapping are optional.
        }

        const bool run_generic_scan = runtime_options.generic_scan_enabled || device_accumulator.Empty();
        if (run_generic_scan) {
            try {
                const auto generic_entries = ReadGenericDeviceBattery(query_reader_context);
                WindowsBatteryProviderDebugLog(
                    "Generic battery entries: " + std::to_string(generic_entries.size()));
                for (const auto& generic_entry : generic_entries) {
                    try_add_entry(generic_entry);
                }
            } catch (const winrt::hresult_error&) {
                // Generic device battery properties may be unavailable.
            }
        } else {
            WindowsBatteryProviderDebugLog(
                "Generic device scan skipped (set BATTERY_MONITOR_GENERIC_SCAN=1 to enable).");
        }

        DisconnectedPairedCollection paired_collection;
        if (options.include_disconnected) {
            try {
                paired_collection = CollectDisconnectedPairedBluetoothEntries(
                    runtime_options.debug_enabled,
                    &WindowsBatteryProviderDebugLog);
                for (auto& entry : paired_collection.offline_entries) {
                    const auto entry_address = ParseBluetoothAddressFromDeviceId(entry.device_id);
                    const bool already_known = std::any_of(
                        device_accumulator.Entries().begin(), device_accumulator.Entries().end(),
                        [&](const DeviceBatteryInfo& existing) {
                            if (existing.device_id == entry.device_id) {
                                return true;
                            }
                            if (!entry_address.has_value()) {
                                return false;
                            }
                            const auto existing_address = ParseBluetoothAddressFromDeviceId(existing.device_id);
                            return existing_address.has_value() && *existing_address == *entry_address;
                        });
                    if (already_known) {
                        continue;
                    }
                    try_add_entry(std::move(entry));
                }
            } catch (const winrt::hresult_error&) {
                // Paired fallback for disconnected devices is best-effort.
            }
        }

        auto devices_with_battery = device_accumulator.TakeEntries();
        ApplyPnpVisualHints(&devices_with_battery);
        const PairedBluetoothSnapshot* paired_snapshot =
            paired_collection.snapshot.loaded ? &paired_collection.snapshot : nullptr;
        return FinalizeCollectedEntries(
            std::move(devices_with_battery),
            options.include_disconnected,
            paired_snapshot);
    } catch (const winrt::hresult_error& error) {
        WindowsBatteryProviderDebugLog(
            "GetConnectedDevicesBattery failed with WinRT error: " +
            DescribeWinrtBatteryProviderError(error));
        return {};
    } catch (const std::exception& error) {
        WindowsBatteryProviderDebugLog(
            std::string("GetConnectedDevicesBattery failed: ") + error.what());
        return {};
    } catch (...) {
        WindowsBatteryProviderDebugLog("GetConnectedDevicesBattery failed with unknown exception.");
        return {};
    }
}

bool WinRtBatteryProvider::SetXiaomiNoiseMode(const std::string& mode, const std::string& device_hint) {
    EnsureWindowsBatteryProviderApartmentInitialized();

    const auto target = ResolveConnectedXiaomiControlTarget(this, device_hint);
    if (!target.has_value()) {
        std::cout << "No connected Xiaomi/Redmi earbuds candidates were found.\n";
        return false;
    }

    return SetXiaomiNoiseModeForTarget(*target, mode, MakeWindowsXiaomiControlActionContext());
}

bool WinRtBatteryProvider::SupportsNoiseControl(const std::string& device_id) {
    return ParseBluetoothAddressFromDeviceId(device_id).has_value();
}

bool WinRtBatteryProvider::SetNoiseControlMode(const std::string& device_id, NoiseControlMode mode) {
    const auto address = ParseBluetoothAddressFromDeviceId(device_id);
    if (!address.has_value()) {
        return false;
    }

    EnsureWindowsBatteryProviderApartmentInitialized();
    return SetNoiseControlModeForAddress(*address, mode, MakeWindowsXiaomiControlActionContext());
}

bool WinRtBatteryProvider::SupportsNoiseSubmodes(const std::string& device_id, NoiseControlMode mode) {
    return SupportsNoiseControl(device_id) && NoiseControlModeSupportsSubmodes(mode);
}

std::vector<std::pair<std::string, std::string>> WinRtBatteryProvider::GetNoiseSubmodes(
    const std::string& device_id,
    NoiseControlMode mode) {
    if (!SupportsNoiseSubmodes(device_id, mode)) {
        return {};
    }

    return GetNoiseControlSubmodes(mode);
}

bool WinRtBatteryProvider::SetNoiseSubmode(const std::string& device_id,
                                           NoiseControlMode mode,
                                           const std::string& submode_id) {
    if (!SupportsNoiseSubmodes(device_id, mode)) {
        return false;
    }

    const std::string normalized_submode = NormalizeNoiseControlToken(submode_id);
    if (mode == NoiseControlMode::Transparency) {
        if (normalized_submode == "standard") {
            return SetXiaomiNoiseSubmode("transparency", 0, device_id);
        }
        if (normalized_submode == "voice") {
            return SetXiaomiNoiseSubmode("transparency", 1, device_id);
        }
        return false;
    }

    if (mode == NoiseControlMode::Anc) {
        if (normalized_submode == "balanced") {
            return SetXiaomiNoiseSubmode("anc", 0, device_id);
        }
        if (normalized_submode == "weak") {
            return SetXiaomiNoiseSubmode("anc", 1, device_id);
        }
        if (normalized_submode == "deep") {
            return SetXiaomiNoiseSubmode("anc", 2, device_id);
        }
        if (normalized_submode == "adaptive") {
            return SetXiaomiNoiseSubmode("anc", 3, device_id);
        }
    }

    return false;
}

bool WinRtBatteryProvider::SetXiaomiNoiseSubmode(const std::string& family,
                                                 int submode,
                                                 const std::string& device_hint) {
    const auto target = ResolveConnectedXiaomiControlTarget(this, device_hint);
    if (!target.has_value()) {
        std::cout << "No connected Xiaomi/Redmi earbuds candidates were found.\n";
        return false;
    }

    return SetXiaomiNoiseSubmodeForTarget(*target, family, submode, MakeWindowsXiaomiControlActionContext());
}

}  // namespace battery_monitor

