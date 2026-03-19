#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <winsock2.h>

#include "platform/windows/XiaomiBatteryCodec.h"

namespace battery_monitor {

using ZmiReplyToHfpAgCommandFn = void (*)(SOCKET socket_handle, const std::string& line);
using ZmiParseAtBatteryPercentFn = std::optional<std::uint8_t> (*)(const std::string& line);
using ZmiSerialDebugLogFn = void (*)(const std::string&);

std::vector<BatteryReading> TryReadZmiSerialBatteryFromSocket(
    SOCKET socket_handle,
    ZmiReplyToHfpAgCommandFn reply_to_hfp_ag_command = nullptr,
    ZmiParseAtBatteryPercentFn parse_at_battery_percent = nullptr,
    bool debug_enabled = false,
    ZmiSerialDebugLogFn debug_log = nullptr,
    int observe_ms = 0);

}  // namespace battery_monitor
