#include "platform/windows/devices/xiaomi/XiaomiProtocol.h"

#include <array>
#include <cstddef>
#include <utility>

namespace battery_monitor {

namespace {

constexpr std::array<std::uint8_t, 3> kXiaomiMessageHeader = {0xFE, 0xDC, 0xBA};
constexpr std::uint8_t kXiaomiMessageTrailer = 0xEF;

}  // namespace

bool IsXiaomiRequestType(XiaomiMessageType type) {
    return (static_cast<std::uint8_t>(type) & 0x40U) != 0U;
}

std::vector<std::uint8_t> EncodeXiaomiMessage(const XiaomiMessage& message) {
    const bool is_request = IsXiaomiRequestType(message.type);
    const std::uint16_t payload_length = static_cast<std::uint16_t>(message.payload.size() + (is_request ? 1U : 2U));

    std::vector<std::uint8_t> bytes;
    bytes.reserve(8U + payload_length);
    bytes.insert(bytes.end(), kXiaomiMessageHeader.begin(), kXiaomiMessageHeader.end());
    bytes.push_back(static_cast<std::uint8_t>(message.type));
    bytes.push_back(static_cast<std::uint8_t>(message.opcode));
    bytes.push_back(static_cast<std::uint8_t>((payload_length >> 8) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>(payload_length & 0xFFU));
    if (!is_request) {
        bytes.push_back(0x00);
    }
    bytes.push_back(message.sequence);
    bytes.insert(bytes.end(), message.payload.begin(), message.payload.end());
    bytes.push_back(kXiaomiMessageTrailer);
    return bytes;
}

bool ParseXiaomiMessage(const std::vector<std::uint8_t>& bytes, XiaomiMessage* message) {
    if (message == nullptr || bytes.size() < 9U) {
        return false;
    }

    if (!(bytes[0] == kXiaomiMessageHeader[0] && bytes[1] == kXiaomiMessageHeader[1] &&
          bytes[2] == kXiaomiMessageHeader[2])) {
        return false;
    }
    if (bytes.back() != kXiaomiMessageTrailer) {
        return false;
    }

    const auto type = static_cast<XiaomiMessageType>(bytes[3]);
    const bool is_request = IsXiaomiRequestType(type);
    const std::uint16_t payload_length = static_cast<std::uint16_t>((bytes[5] << 8) | bytes[6]);
    const std::size_t expected_size = 8U + payload_length;
    if (bytes.size() != expected_size) {
        return false;
    }

    const std::size_t sequence_offset = is_request ? 7U : 8U;
    const std::size_t payload_offset = sequence_offset + 1U;
    const std::size_t overhead = is_request ? 1U : 2U;
    if (payload_length < overhead) {
        return false;
    }
    const std::size_t actual_payload_length = payload_length - overhead;
    if (payload_offset + actual_payload_length + 1U != bytes.size()) {
        return false;
    }

    message->type = type;
    message->opcode = static_cast<XiaomiOpcode>(bytes[4]);
    message->sequence = bytes[sequence_offset];
    message->payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset),
                            bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset + actual_payload_length));
    return true;
}

std::vector<XiaomiMessage> DecodeXiaomiMessages(std::vector<std::uint8_t>* buffer) {
    std::vector<XiaomiMessage> messages;
    if (buffer == nullptr) {
        return messages;
    }

    std::size_t cursor = 0;
    while (cursor + 8U <= buffer->size()) {
        if (!((*buffer)[cursor] == kXiaomiMessageHeader[0] && (*buffer)[cursor + 1U] == kXiaomiMessageHeader[1] &&
              (*buffer)[cursor + 2U] == kXiaomiMessageHeader[2])) {
            ++cursor;
            continue;
        }

        if (cursor + 7U >= buffer->size()) {
            break;
        }

        const std::uint16_t payload_length =
            static_cast<std::uint16_t>(((*buffer)[cursor + 5U] << 8) | (*buffer)[cursor + 6U]);
        const std::size_t total_length = 8U + payload_length;
        if (cursor + total_length > buffer->size()) {
            break;
        }

        std::vector<std::uint8_t> chunk(buffer->begin() + static_cast<std::ptrdiff_t>(cursor),
                                        buffer->begin() + static_cast<std::ptrdiff_t>(cursor + total_length));
        XiaomiMessage parsed;
        if (ParseXiaomiMessage(chunk, &parsed)) {
            messages.push_back(std::move(parsed));
        }
        cursor += total_length;
    }

    if (cursor > 0U) {
        buffer->erase(buffer->begin(), buffer->begin() + static_cast<std::ptrdiff_t>(cursor));
    }

    return messages;
}

}  // namespace battery_monitor

