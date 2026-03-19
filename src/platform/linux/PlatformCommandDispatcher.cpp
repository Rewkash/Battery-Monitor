#include "app/PlatformCommandDispatcher.h"

namespace battery_monitor {

std::optional<int> TryRunPlatformCommand(const PlatformCommandOptions& options) {
    (void)options;
    return std::nullopt;
}

bool PrintPlatformException(const std::exception& exception, std::ostream& stream) {
    (void)exception;
    (void)stream;
    return false;
}

}  // namespace battery_monitor
