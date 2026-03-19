#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <winsock2.h>

#include "platform/windows/devices/xiaomi/XiaomiProtocol.h"

namespace battery_monitor {

std::vector<XiaomiMessage> AppendAndDecodeXiaomiMessages(std::vector<std::uint8_t>* rx_buffer,
                                                         const std::vector<std::uint8_t>& chunk);
std::string FormatXiaomiChunkLine(const std::vector<std::uint8_t>& chunk);
std::string FormatXiaomiMessageLine(const XiaomiMessage& message,
                                    bool include_sequence,
                                    std::string_view prefix = "rx ");
void SendXiaomiInfoRequests(SOCKET socket_handle, std::uint8_t* sequence, bool run_info_first = false);
bool IsXiaomiReportStatusNotification(const XiaomiMessage& message);
bool SendXiaomiReportStatusAck(SOCKET socket_handle, const XiaomiMessage& message);

}  // namespace battery_monitor

