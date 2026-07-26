#include "update/StartupHealth.h"

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
        if (end != argv[index + 1] && *end == '\0' && raw != 0) {
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
