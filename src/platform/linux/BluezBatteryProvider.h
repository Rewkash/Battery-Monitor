#pragma once

#include "core/IBluetoothBatteryProvider.h"

namespace battery_monitor {

class BluezBatteryProvider final : public IBluetoothBatteryProvider {
   public:
    std::vector<DeviceBatteryInfo> GetConnectedDevicesBattery() override;
};

}  // namespace battery_monitor

