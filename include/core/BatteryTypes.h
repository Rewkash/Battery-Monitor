#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace battery_monitor {

struct DeviceBatteryInfo {
    std::string device_id;
    std::string device_name;
    std::string battery_component = "main";
    std::optional<std::uint8_t> battery_level_percent;
    std::optional<std::string> device_mode;
    std::optional<std::string> device_submode;
    std::optional<std::uint16_t> bluetooth_le_appearance;
    std::optional<std::uint32_t> bluetooth_cod_major;
    std::optional<std::uint32_t> bluetooth_cod_minor;
    std::vector<std::string> device_categories;
    bool is_cached = false;
    bool is_connected = true;
};

struct BatteryQueryOptions {
    bool include_disconnected = false;
    std::string target_device_id;
};

}  // namespace battery_monitor
