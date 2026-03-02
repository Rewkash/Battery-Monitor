#pragma once

#include <cstdint>
#include <string>

namespace battery_monitor {

struct DeviceBatteryInfo {
    std::string device_id;
    std::string device_name;
    std::uint8_t battery_level_percent = 0;
};

}  // namespace battery_monitor

