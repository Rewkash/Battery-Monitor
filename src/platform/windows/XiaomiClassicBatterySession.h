#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <winsock2.h>

#include "platform/windows/XiaomiBatteryCodec.h"
#include "platform/windows/XiaomiHandshake.h"

namespace battery_monitor {

using XiaomiModeCacheUpdateFn = void (*)(std::uint64_t address,
                                         std::uint8_t mode_code,
                                         std::optional<std::uint8_t> submode_code);

struct XiaomiClassicBatterySessionResult {
    std::vector<BatteryReading> readings;
    bool auth_start_sent = false;
};

XiaomiClassicBatterySessionResult RunXiaomiClassicBatterySession(
    SOCKET socket_handle,
    std::uint64_t bluetooth_address,
    XiaomiModeCacheUpdateFn mode_cache_update = nullptr,
    bool debug_enabled = false,
    XiaomiDebugLogFn debug_log = nullptr);

}  // namespace battery_monitor
