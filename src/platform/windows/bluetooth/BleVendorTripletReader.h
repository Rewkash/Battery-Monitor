#pragma once

#include <string>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.h>

#include "platform/windows/devices/xiaomi/XiaomiBatteryCodec.h"

namespace battery_monitor {

using BleVendorTripletDebugLogFn = void (*)(const std::string&);

std::vector<BatteryReading> TryReadBleVendorTripletBattery(
    const winrt::Windows::Devices::Bluetooth::BluetoothLEDevice& device,
    bool debug_enabled = false,
    BleVendorTripletDebugLogFn debug_log = nullptr);

}  // namespace battery_monitor

