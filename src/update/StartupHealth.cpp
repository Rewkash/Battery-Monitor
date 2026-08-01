#include "update/StartupHealth.h"

#include "BatteryMonitorVersion.h"

#ifdef _WIN32
#include <windows.h>

#include <cstdlib>
#include <string_view>
#endif

namespace battery_monitor {

void SignalUpdateStartupHealth(int argc, char** argv) {
#ifdef _WIN32
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string_view(argv[index]) != "--update-health-handle") {
            continue;
        }
        char* end = nullptr;
        const unsigned long long raw = std::strtoull(argv[index + 1], &end, 10);
        bool expected_version_matches = true;
        for (int version_index = 1; version_index + 1 < argc; ++version_index) {
            if (std::string_view(argv[version_index]) == "--update-health-version") {
                expected_version_matches =
                    std::string_view(argv[version_index + 1]) == BATTERY_MONITOR_VERSION;
                break;
            }
        }
        if (expected_version_matches && end != argv[index + 1] && *end == '\0' && raw != 0) {
            HANDLE event = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(raw));
            SetEvent(event);
            CloseHandle(event);
        }
        return;
    }
#else
    (void)argc;
    (void)argv;
#endif
}

}  // namespace battery_monitor
