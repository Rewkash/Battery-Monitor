#pragma once

#include <cstdint>
#include <string>

#include <winsock2.h>

namespace battery_monitor {

using XiaomiDebugLogFn = void (*)(const std::string&);

bool RunXiaomiAuthHandshake(SOCKET socket_handle,
                            std::uint8_t* next_sequence,
                            bool debug_enabled = false,
                            XiaomiDebugLogFn debug_log = nullptr);

}  // namespace battery_monitor
