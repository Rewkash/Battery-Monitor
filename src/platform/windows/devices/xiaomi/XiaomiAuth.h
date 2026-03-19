#pragma once

#include <array>
#include <cstdint>

namespace battery_monitor {

std::array<std::uint8_t, 16> ComputeXiaomiChallengeResponse(const std::array<std::uint8_t, 16>& challenge);
std::array<std::uint8_t, 16> GenerateRandomChallenge();

}  // namespace battery_monitor
