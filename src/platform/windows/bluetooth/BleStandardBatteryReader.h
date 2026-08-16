#pragma once

#include <vector>

#include <winrt/Windows.Devices.Bluetooth.h>

#include "core/BatteryTypes.h"
#include "platform/windows/devices/xiaomi/XiaomiBatteryCodec.h"

namespace battery_monitor {

std::vector<BatteryReading> ReadBleBatteryReadings(
    const winrt::Windows::Devices::Bluetooth::BluetoothLEDevice& device,
    bool prefer_tws_labels,
    const ProviderOperationContext& operation = {});

}  // namespace battery_monitor

