#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/base.h>

#include "core/IBluetoothBatteryProvider.h"
#include "platform/windows/WindowsBatteryQueryReaders.h"
#include "platform/windows/WindowsBleCandidateBatteryCollector.h"
#include "platform/windows/WindowsBluetoothTargetResolver.h"
#include "platform/windows/WindowsTwsCandidateBatteryCollector.h"
#include "platform/windows/XiaomiControlActions.h"

namespace battery_monitor {

struct WindowsBatteryProviderRuntimeOptions {
    bool debug_enabled = false;
    bool generic_scan_enabled = false;
    bool force_aep_scan = false;
    bool persistent_xiaomi_cache_write_enabled = true;
    bool persistent_xiaomi_cache_read_enabled = true;
    int xiaomi_cache_ttl_minutes = 180;
    int xiaomi_advertisement_scan_ms = 1800;
    int zmi_observe_ms = 0;
    std::filesystem::path xiaomi_cache_file_path;
};

const WindowsBatteryProviderRuntimeOptions& GetWindowsBatteryProviderRuntimeOptions();
void WindowsBatteryProviderDebugLog(const std::string& message);
std::string DescribeWinrtBatteryProviderError(const winrt::hresult_error& error);
void EnsureWindowsBatteryProviderApartmentInitialized();

WindowsBatteryQueryReaderContext MakeWindowsBatteryQueryReaderContext();
WindowsBleCandidateBatteryCollectorContext MakeWindowsBleCandidateBatteryCollectorContext();
WindowsTwsCandidateBatteryCollectorContext MakeWindowsTwsCandidateBatteryCollectorContext(
    bool include_disconnected);
XiaomiControlActionContext MakeWindowsXiaomiControlActionContext();

std::optional<winrt::Windows::Devices::Bluetooth::BluetoothLEDevice> TryOpenBleDeviceByAddress(
    std::uint64_t address,
    std::chrono::milliseconds timeout);

std::optional<ResolvedBluetoothTarget> ResolveConnectedXiaomiControlTarget(
    IBluetoothBatteryProvider* provider,
    const std::string& device_hint);
std::optional<ResolvedBluetoothTarget> ResolveConnectedZmiControlTarget(
    IBluetoothBatteryProvider* provider,
    const std::string& device_hint);
std::optional<ResolvedBluetoothTarget> ResolveAnyBluetoothTarget(
    IBluetoothBatteryProvider* provider,
    const std::string& device_hint,
    bool include_disconnected);

}  // namespace battery_monitor
