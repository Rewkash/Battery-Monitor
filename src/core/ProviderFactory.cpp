#include "core/ProviderFactory.h"

#include <stdexcept>

#if defined(_WIN32)
#include "platform/windows/WinRtBatteryProvider.h"
#elif defined(__linux__)
#include "platform/linux/BluezBatteryProvider.h"
#endif

namespace battery_monitor {

std::unique_ptr<IBluetoothBatteryProvider> CreateBatteryProvider() {
#if defined(_WIN32)
    return std::make_unique<WinRtBatteryProvider>();
#elif defined(__linux__)
    return std::make_unique<BluezBatteryProvider>();
#else
    throw std::runtime_error("Bluetooth battery provider is not available on this platform.");
#endif
}

}  // namespace battery_monitor

