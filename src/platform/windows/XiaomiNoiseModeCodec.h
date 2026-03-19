#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace battery_monitor {

std::optional<std::uint8_t> ParseXiaomiNoiseModeCode(std::uint8_t opcode,
                                                     const std::vector<std::uint8_t>& payload);
std::optional<std::uint8_t> ParseXiaomiNoiseSubmodeCodeFromF4Payload(
    const std::vector<std::uint8_t>& payload);
std::string XiaomiNoiseModeCodeToText(std::uint8_t code);
std::optional<std::string> XiaomiNoiseSubmodeCodeToText(std::uint8_t mode_code,
                                                        std::uint8_t submode_code);

}  // namespace battery_monitor
