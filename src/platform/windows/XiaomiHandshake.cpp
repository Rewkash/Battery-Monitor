#include "platform/windows/XiaomiHandshake.h"

#include "platform/windows/BluetoothSocketUtils.h"
#include "platform/windows/XiaomiAuth.h"
#include "platform/windows/XiaomiControlSession.h"
#include "platform/windows/XiaomiProtocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace battery_monitor {

namespace {

void DebugLogHandshakeMessage(bool debug_enabled,
                              XiaomiDebugLogFn debug_log,
                              const XiaomiMessage& message) {
    if (!debug_enabled || debug_log == nullptr) {
        return;
    }

    debug_log(FormatXiaomiMessageLine(message, true, "Probe auth rx "));
}

}  // namespace

bool RunXiaomiAuthHandshake(SOCKET socket_handle,
                            std::uint8_t* next_sequence,
                            bool debug_enabled,
                            XiaomiDebugLogFn debug_log) {
    if (socket_handle == INVALID_SOCKET || next_sequence == nullptr) {
        return false;
    }

    std::uint8_t sequence = 0;
    const auto challenge = GenerateRandomChallenge();
    XiaomiMessage auth_start;
    auth_start.type = XiaomiMessageType::kPhoneRequest;
    auth_start.opcode = XiaomiOpcode::kAuthChallenge;
    auth_start.sequence = sequence++;
    auth_start.payload.push_back(0x01);
    auth_start.payload.insert(auth_start.payload.end(), challenge.begin(), challenge.end());
    if (!SendAll(socket_handle, EncodeXiaomiMessage(auth_start))) {
        return false;
    }

    std::vector<std::uint8_t> rx_buffer;
    bool init_requests_sent = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1800);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto chunk = ReceiveChunk(socket_handle);
        if (!chunk.has_value()) {
            continue;
        }

        const auto messages = AppendAndDecodeXiaomiMessages(&rx_buffer, *chunk);
        for (const auto& message : messages) {
            DebugLogHandshakeMessage(debug_enabled, debug_log, message);

            if (message.opcode == XiaomiOpcode::kAuthChallenge) {
                if (message.type == XiaomiMessageType::kResponse) {
                    XiaomiMessage confirm;
                    confirm.type = XiaomiMessageType::kPhoneRequest;
                    confirm.opcode = XiaomiOpcode::kAuthConfirm;
                    confirm.sequence = sequence++;
                    confirm.payload = {0x01, 0x00};
                    SendAll(socket_handle, EncodeXiaomiMessage(confirm));
                    continue;
                }

                if (message.type == XiaomiMessageType::kEarbudsRequest && message.payload.size() >= 17U) {
                    std::array<std::uint8_t, 16> remote_challenge{};
                    std::copy_n(message.payload.begin() + 1, 16, remote_challenge.begin());
                    const auto response = ComputeXiaomiChallengeResponse(remote_challenge);

                    XiaomiMessage challenge_response;
                    challenge_response.type = XiaomiMessageType::kResponse;
                    challenge_response.opcode = XiaomiOpcode::kAuthChallenge;
                    challenge_response.sequence = message.sequence;
                    challenge_response.payload.push_back(0x01);
                    challenge_response.payload.insert(challenge_response.payload.end(), response.begin(), response.end());
                    SendAll(socket_handle, EncodeXiaomiMessage(challenge_response));
                    continue;
                }
            }

            if (message.opcode == XiaomiOpcode::kAuthConfirm && message.type == XiaomiMessageType::kEarbudsRequest) {
                XiaomiMessage ack;
                ack.type = XiaomiMessageType::kResponse;
                ack.opcode = XiaomiOpcode::kAuthConfirm;
                ack.sequence = message.sequence;
                ack.payload = {0x01};
                SendAll(socket_handle, EncodeXiaomiMessage(ack));

                if (!init_requests_sent) {
                    SendXiaomiInfoRequests(socket_handle, &sequence);
                    init_requests_sent = true;
                }

                *next_sequence = sequence;
                return true;
            }
        }
    }

    return false;
}

}  // namespace battery_monitor
