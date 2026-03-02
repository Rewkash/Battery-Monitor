#pragma once

#include <vector>

#include "core/BatteryTypes.h"

namespace battery_monitor {

class IBluetoothBatteryProvider {
   public:
    virtual ~IBluetoothBatteryProvider() = default;

    virtual std::vector<DeviceBatteryInfo> GetConnectedDevicesBattery() = 0;
};

}  // namespace battery_monitor

