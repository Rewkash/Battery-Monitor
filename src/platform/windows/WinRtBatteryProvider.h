#pragma once

#include "core/IBluetoothBatteryProvider.h"

namespace battery_monitor {

class WinRtBatteryProvider final : public IBluetoothBatteryProvider {
   public:
    std::vector<DeviceBatteryInfo> GetDevicesBattery(const BatteryQueryOptions& options) override;
};

}  // namespace battery_monitor
