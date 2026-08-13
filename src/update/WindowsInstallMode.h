#pragma once

namespace battery_monitor {

enum class WindowsInstallMode {
    Portable,
    PerUserMsi,
};

[[nodiscard]] WindowsInstallMode DetectWindowsInstallMode();

}  // namespace battery_monitor
