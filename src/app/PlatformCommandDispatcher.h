#pragma once

#include <exception>
#include <iosfwd>
#include <optional>

#include "app/CommandLineOptions.h"

namespace battery_monitor {

[[nodiscard]] std::optional<int> TryRunPlatformCommand(const PlatformCommandOptions& options);
[[nodiscard]] bool PrintPlatformException(const std::exception& exception, std::ostream& stream);

}  // namespace battery_monitor
