#pragma once

#include <cstdint>
#include <vector>

namespace battery_monitor {

enum class XiaomiMessageType : std::uint8_t {
    kPhoneRequest = 0xC4,
    kResponse = 0x04,
    kEarbudsRequest = 0xC0,
    kEarbudsNotify = 0xC7
};

enum class XiaomiOpcode : std::uint8_t {
    kGetDeviceInfo = 0x02,
    kGetDeviceRunInfo = 0x09,
    kReportStatus = 0x0E,
    kAuthChallenge = 0x50,
    kAuthConfirm = 0x51
};

struct XiaomiMessage {
    XiaomiMessageType type = XiaomiMessageType::kPhoneRequest;
    XiaomiOpcode opcode = XiaomiOpcode::kGetDeviceInfo;
    std::uint8_t sequence = 0;
    std::vector<std::uint8_t> payload;
};

struct XiaomiProbeCommand {
    const char* label = "";
    XiaomiMessageType type = XiaomiMessageType::kPhoneRequest;
    std::uint8_t opcode = 0;
    std::vector<std::uint8_t> payload;
    int pause_ms = 1800;
};

bool IsXiaomiRequestType(XiaomiMessageType type);
std::vector<std::uint8_t> EncodeXiaomiMessage(const XiaomiMessage& message);
bool ParseXiaomiMessage(const std::vector<std::uint8_t>& bytes, XiaomiMessage* message);
std::vector<XiaomiMessage> DecodeXiaomiMessages(std::vector<std::uint8_t>* buffer);
std::vector<XiaomiProbeCommand> BuildXiaomiNoiseProbeCommands();

}  // namespace battery_monitor
