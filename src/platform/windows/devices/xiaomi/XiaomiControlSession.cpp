#include "platform/windows/devices/xiaomi/XiaomiControlSession.h"

#include "platform/windows/bluetooth/BluetoothSocketUtils.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace battery_monitor {

namespace {

std::string BytesToHex(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return {};
    }

    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(bytes.size() * 3U);

    for (const auto value : bytes) {
        output.push_back(kHex[(value >> 4U) & 0x0FU]);
        output.push_back(kHex[value & 0x0FU]);
        output.push_back(' ');
    }

    return output;
}

std::string ByteToHex(std::uint8_t value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string output = "0x";
    output.push_back(kHex[(value >> 4U) & 0x0FU]);
    output.push_back(kHex[value & 0x0FU]);
    return output;
}

void SendOneXiaomiInfoRequest(SOCKET socket_handle,
                              XiaomiOpcode opcode,
                              std::uint8_t* sequence) {
    if (sequence == nullptr) {
        return;
    }

    XiaomiMessage request;
    request.type = XiaomiMessageType::kPhoneRequest;
    request.opcode = opcode;
    request.sequence = (*sequence)++;
    request.payload = {0xFF, 0xFF, 0xFF, 0xFF};
    SendAll(socket_handle, EncodeXiaomiMessage(request));
}

}  // namespace

std::vector<XiaomiMessage> AppendAndDecodeXiaomiMessages(std::vector<std::uint8_t>* rx_buffer,
                                                         const std::vector<std::uint8_t>& chunk) {
    if (rx_buffer == nullptr) {
        return {};
    }

    rx_buffer->insert(rx_buffer->end(), chunk.begin(), chunk.end());
    return DecodeXiaomiMessages(rx_buffer);
}

std::string FormatXiaomiChunkLine(const std::vector<std::uint8_t>& chunk) {
    return "chunk: " + BytesToHex(chunk);
}

std::string FormatXiaomiMessageLine(const XiaomiMessage& message,
                                    bool include_sequence,
                                    std::string_view prefix) {
    std::string output(prefix);
    output += "type=" + ByteToHex(static_cast<std::uint8_t>(message.type));
    output += " opcode=" + ByteToHex(static_cast<std::uint8_t>(message.opcode));
    if (include_sequence) {
        output += " seq=" + std::to_string(message.sequence);
    }
    output += " payload=" + BytesToHex(message.payload);
    return output;
}

void SendXiaomiInfoRequests(SOCKET socket_handle, std::uint8_t* sequence, bool run_info_first) {
    if (run_info_first) {
        SendOneXiaomiInfoRequest(socket_handle, XiaomiOpcode::kGetDeviceRunInfo, sequence);
        SendOneXiaomiInfoRequest(socket_handle, XiaomiOpcode::kGetDeviceInfo, sequence);
        return;
    }

    SendOneXiaomiInfoRequest(socket_handle, XiaomiOpcode::kGetDeviceInfo, sequence);
    SendOneXiaomiInfoRequest(socket_handle, XiaomiOpcode::kGetDeviceRunInfo, sequence);
}

bool IsXiaomiReportStatusNotification(const XiaomiMessage& message) {
    return message.type == XiaomiMessageType::kEarbudsNotify &&
           message.opcode == XiaomiOpcode::kReportStatus;
}

bool SendXiaomiReportStatusAck(SOCKET socket_handle, const XiaomiMessage& message) {
    if (!IsXiaomiReportStatusNotification(message)) {
        return false;
    }

    XiaomiMessage ack;
    ack.type = XiaomiMessageType::kResponse;
    ack.opcode = XiaomiOpcode::kReportStatus;
    ack.sequence = message.sequence;
    ack.payload = {};
    return SendAll(socket_handle, EncodeXiaomiMessage(ack));
}

}  // namespace battery_monitor

