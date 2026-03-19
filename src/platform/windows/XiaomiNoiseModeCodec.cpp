#include "platform/windows/XiaomiNoiseModeCodec.h"

#include "platform/windows/XiaomiProtocol.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace battery_monitor {

namespace {

std::optional<std::uint8_t> ParseXiaomiNoiseModeCodeFromRunInfoPayload(
    const std::vector<std::uint8_t>& payload) {
    for (std::size_t index = 0; index + 2U < payload.size();) {
        const std::size_t len = payload[index];
        const std::size_t field_size = len + 1U;
        if (len < 2U || index + field_size > payload.size()) {
            break;
        }

        const std::uint8_t tag = payload[index + 1U];
        if (tag == 0x09U && len >= 2U) {
            return payload[index + 2U];
        }
        index += field_size;
    }

    return std::nullopt;
}

std::optional<std::uint8_t> ParseXiaomiNoiseModeCodeFromStatusPayload(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() >= 3U && payload[0] == 0x02U && payload[1] == 0x04U) {
        return payload[2];
    }

    return std::nullopt;
}

std::optional<std::uint8_t> ParseXiaomiNoiseModeCodeFromF4Payload(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() >= 4U && payload[0] == 0x04U && payload[1] == 0x00U && payload[2] == 0x0BU) {
        return payload[3];
    }

    return std::nullopt;
}

}  // namespace

std::optional<std::uint8_t> ParseXiaomiNoiseModeCode(std::uint8_t opcode,
                                                     const std::vector<std::uint8_t>& payload) {
    if (opcode == static_cast<std::uint8_t>(XiaomiOpcode::kGetDeviceRunInfo)) {
        return ParseXiaomiNoiseModeCodeFromRunInfoPayload(payload);
    }
    if (opcode == static_cast<std::uint8_t>(XiaomiOpcode::kReportStatus)) {
        return ParseXiaomiNoiseModeCodeFromStatusPayload(payload);
    }
    if (opcode == 0xF4U) {
        return ParseXiaomiNoiseModeCodeFromF4Payload(payload);
    }

    return std::nullopt;
}

std::optional<std::uint8_t> ParseXiaomiNoiseSubmodeCodeFromF4Payload(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() >= 5U && payload[0] == 0x04U && payload[1] == 0x00U && payload[2] == 0x0BU) {
        return payload[4];
    }

    return std::nullopt;
}

std::string XiaomiNoiseModeCodeToText(std::uint8_t code) {
    if (code == 0U) {
        return "off";
    }
    if (code == 2U) {
        return "transparency";
    }
    if (code == 1U) {
        return "anc";
    }

    return "mode " + std::to_string(code);
}

std::optional<std::string> XiaomiNoiseSubmodeCodeToText(std::uint8_t mode_code,
                                                        std::uint8_t submode_code) {
    if (mode_code == 1U) {
        if (submode_code == 0U) {
            return std::string("balanced");
        }
        if (submode_code == 1U) {
            return std::string("weak");
        }
        if (submode_code == 2U) {
            return std::string("deep");
        }
        if (submode_code == 3U) {
            return std::string("adaptive");
        }
    }
    if (mode_code == 2U) {
        if (submode_code == 0U) {
            return std::string("standard");
        }
        if (submode_code == 1U) {
            return std::string("voice");
        }
    }

    return std::nullopt;
}

}  // namespace battery_monitor
