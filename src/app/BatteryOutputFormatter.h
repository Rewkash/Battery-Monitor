#pragma once

#include <iosfwd>
#include <vector>

#include "core/BatteryTypes.h"

namespace battery_monitor {

enum class DeviceListOutputFormat {
    Table,
    Json,
};

void PrintDevices(const std::vector<DeviceBatteryInfo>& devices, DeviceListOutputFormat format, std::ostream& stream);

}  // namespace battery_monitor
