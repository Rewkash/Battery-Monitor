#pragma once

#include <vector>

#include "core/BatteryTypes.h"

namespace battery_monitor {

class INoiseControlProvider;

class IBluetoothBatteryProvider {
   public:
    virtual ~IBluetoothBatteryProvider() = default;

    virtual std::vector<DeviceBatteryInfo> GetDevicesBattery(const BatteryQueryOptions& options) = 0;
    virtual INoiseControlProvider* GetNoiseControlProvider() { return nullptr; }

    std::vector<DeviceBatteryInfo> GetConnectedDevicesBattery() {
        return GetDevicesBattery(BatteryQueryOptions{});
    }
};

}  // namespace battery_monitor
