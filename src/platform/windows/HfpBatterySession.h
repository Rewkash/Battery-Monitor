#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <winsock2.h>

#include "platform/windows/XiaomiBatteryCodec.h"

namespace battery_monitor {

using HfpDebugLogFn = void (*)(const std::string&);

void ReplyToHfpAgCommand(SOCKET socket_handle, const std::string& line);
std::optional<std::uint8_t> ParseAtBatteryPercentFromLine(const std::string& line);
std::optional<std::uint8_t> TryReadHfpBatteryFromSocket(SOCKET socket_handle,
                                                        bool debug_enabled = false,
                                                        HfpDebugLogFn debug_log = nullptr);
std::vector<BatteryReading> TryReadHfpAgBatteryFromSocket(SOCKET socket_handle,
                                                          bool debug_enabled = false,
                                                          HfpDebugLogFn debug_log = nullptr);

}  // namespace battery_monitor
