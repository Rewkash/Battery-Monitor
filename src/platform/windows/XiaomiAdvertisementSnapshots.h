#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include <winrt/Windows.Devices.Bluetooth.h>

#include "platform/windows/XiaomiBatteryCodec.h"
#include "platform/windows/XiaomiHandshake.h"

namespace battery_monitor {

using OpenBleDeviceByAddressFn =
    std::optional<winrt::Windows::Devices::Bluetooth::BluetoothLEDevice> (*)(
        std::uint64_t, std::chrono::milliseconds);

struct AdvertisementSnapshotResult {
    std::unordered_map<std::uint64_t, XiaomiBatterySnapshot> by_address;
    std::unordered_map<std::string, XiaomiBatterySnapshot> by_name;
    std::unordered_map<std::uint16_t, XiaomiBatterySnapshot> by_product_id;
};

AdvertisementSnapshotResult ScanXiaomiAdvertisementSnapshots(std::chrono::milliseconds scan_duration,
                                                             OpenBleDeviceByAddressFn open_ble_device,
                                                             bool debug_enabled = false,
                                                             XiaomiDebugLogFn debug_log = nullptr);

}  // namespace battery_monitor
