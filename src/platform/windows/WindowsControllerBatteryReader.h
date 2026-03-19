#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace battery_monitor {

using ControllerDebugLogFn = void (*)(const std::string&);

std::optional<std::uint8_t> ReadControllerBatteryCached(const std::string& device_name_hint,
                                                        const std::string& device_id_hint,
                                                        bool debug_enabled = false,
                                                        ControllerDebugLogFn debug_log = nullptr);

}  // namespace battery_monitor
