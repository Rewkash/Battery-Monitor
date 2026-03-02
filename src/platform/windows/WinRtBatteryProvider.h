#pragma once

#include "core/IBluetoothBatteryProvider.h"

namespace battery_monitor {

class WinRtBatteryProvider final : public IBluetoothBatteryProvider {
   public:
    std::vector<DeviceBatteryInfo> GetConnectedDevicesBattery() override;
};

}  // namespace battery_monitor

