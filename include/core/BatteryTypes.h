#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace battery_monitor {

struct DeviceBatteryInfo {
    std::string device_id;
    std::string device_name;
    std::string battery_component = "main";
    std::optional<std::uint8_t> battery_level_percent;
};

}  // namespace battery_monitor
