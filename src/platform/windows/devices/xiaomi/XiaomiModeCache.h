#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace battery_monitor {

void PutXiaomiModeCacheEntry(std::uint64_t address,
                             std::uint8_t code,
                             std::optional<std::uint8_t> submode_code = std::nullopt);
std::optional<std::string> TryGetXiaomiModeCacheEntry(std::uint64_t address);
std::optional<std::string> TryGetXiaomiSubmodeCacheEntry(std::uint64_t address);

}  // namespace battery_monitor
