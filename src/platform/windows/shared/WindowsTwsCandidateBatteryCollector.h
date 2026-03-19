#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include <winrt/Windows.Devices.Bluetooth.h>

#include "platform/windows/shared/WindowsBatteryAggregation.h"
#include "platform/windows/shared/WindowsBatteryQueryReaders.h"
#include "platform/windows/devices/xiaomi/XiaomiBatteryCaches.h"

namespace battery_monitor {

using TwsDeviceHeuristicFn = bool (*)(const std::string&, const std::string&, const std::string&);
using OpenBleDeviceByAddressFn =
    std::optional<winrt::Windows::Devices::Bluetooth::BluetoothLEDevice> (*)(std::uint64_t,
                                                                              std::chrono::milliseconds);

struct WindowsTwsCandidateBatteryCollectorContext {
    bool debug_enabled = false;
    XiaomiDebugLogFn debug_log = nullptr;
    bool include_disconnected = false;
    bool force_aep_scan = false;
    TwsDeviceHeuristicFn is_likely_xiaomi_earbuds = nullptr;
    TwsDeviceHeuristicFn should_aggressive_xiaomi_classic_retry = nullptr;
    OpenBleDeviceByAddressFn open_ble_device_by_address = nullptr;
};

void CollectTwsCandidateBatteryEntries(const WindowsTwsCandidateBatteryCollectorContext& context,
                                       const WindowsBatteryQueryReaderContext& query_reader_context,
                                       DeviceBatteryAccumulator* accumulator,
                                       XiaomiClassicBatteryCache* xiaomi_classic_cache);

}  // namespace battery_monitor

