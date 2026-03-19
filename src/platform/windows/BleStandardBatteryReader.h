#pragma once

#include <vector>

#include <winrt/Windows.Devices.Bluetooth.h>

#include "platform/windows/XiaomiBatteryCodec.h"

namespace battery_monitor {

std::vector<BatteryReading> ReadBleBatteryReadings(
    const winrt::Windows::Devices::Bluetooth::BluetoothLEDevice& device,
    bool prefer_tws_labels);

}  // namespace battery_monitor
