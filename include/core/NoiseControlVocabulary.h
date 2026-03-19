#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/INoiseControlProvider.h"

namespace battery_monitor {

std::string NormalizeNoiseControlToken(std::string_view value);
std::optional<NoiseControlMode> ParseNoiseControlModeToken(std::string_view value);
std::string NoiseControlModeToken(NoiseControlMode mode);
bool NoiseControlModeSupportsSubmodes(NoiseControlMode mode) noexcept;
std::vector<std::pair<std::string, std::string>> GetNoiseControlSubmodes(NoiseControlMode mode);
std::string GetDefaultNoiseControlSubmodeToken(NoiseControlMode mode);

}  // namespace battery_monitor
