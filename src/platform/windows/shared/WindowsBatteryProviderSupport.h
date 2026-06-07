#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/base.h>

#include "core/IBluetoothBatteryProvider.h"
#include "platform/windows/shared/WindowsBatteryQueryReaders.h"
#include "platform/windows/shared/WindowsBleCandidateBatteryCollector.h"
#include "platform/windows/shared/WindowsBluetoothTargetResolver.h"
#include "platform/windows/shared/WindowsTwsCandidateBatteryCollector.h"
#include "platform/windows/devices/xiaomi/XiaomiControlActions.h"

namespace battery_monitor {

struct WindowsBatteryProviderRuntimeOptions {
    bool debug_enabled = false;
    bool generic_scan_enabled = false;
    bool force_aep_scan = false;
    bool persistent_xiaomi_cache_write_enabled = true;
    bool persistent_xiaomi_cache_read_enabled = true;
    int xiaomi_cache_ttl_minutes = 180;
    std::filesystem::path xiaomi_cache_file_path;
};

const WindowsBatteryProviderRuntimeOptions& GetWindowsBatteryProviderRuntimeOptions();
void WindowsBatteryProviderDebugLog(const std::string& message);
std::string DescribeWinrtBatteryProviderError(const winrt::hresult_error& error);
void EnsureWindowsBatteryProviderApartmentInitialized();

WindowsBatteryQueryReaderContext MakeWindowsBatteryQueryReaderContext();
WindowsBleCandidateBatteryCollectorContext MakeWindowsBleCandidateBatteryCollectorContext(
    const std::string& target_device_id = std::string());
WindowsTwsCandidateBatteryCollectorContext MakeWindowsTwsCandidateBatteryCollectorContext(
    bool include_disconnected,
    const std::string& target_device_id = std::string());
XiaomiControlActionContext MakeWindowsXiaomiControlActionContext();

std::optional<winrt::Windows::Devices::Bluetooth::BluetoothLEDevice> TryOpenBleDeviceByAddress(
    std::uint64_t address,
    std::chrono::milliseconds timeout);
bool DeviceIdMatchesBluetoothTarget(std::string_view device_id, std::string_view target_device_id);

std::optional<ResolvedBluetoothTarget> ResolveConnectedXiaomiControlTarget(
    IBluetoothBatteryProvider* provider,
    const std::string& device_hint);

}  // namespace battery_monitor

