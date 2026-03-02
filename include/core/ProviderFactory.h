#pragma once

#include <memory>

#include "core/IBluetoothBatteryProvider.h"

namespace battery_monitor {

std::unique_ptr<IBluetoothBatteryProvider> CreateBatteryProvider();

}  // namespace battery_monitor

