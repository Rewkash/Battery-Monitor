#include "platform/windows/devices/xiaomi/XiaomiClassicBatterySession.h"

#include "platform/windows/bluetooth/BluetoothSocketUtils.h"
#include "platform/windows/devices/xiaomi/XiaomiAuth.h"
#include "platform/windows/devices/xiaomi/XiaomiControlSession.h"
#include "platform/windows/devices/xiaomi/XiaomiNoiseModeCodec.h"
#include "platform/windows/devices/xiaomi/XiaomiProtocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace battery_monitor {

namespace {

void MaybeDebugLog(bool debug_enabled, XiaomiDebugLogFn debug_log, const std::string& message) {
    if (debug_enabled && debug_log != nullptr) {
        debug_log(message);
    }
}

std::optional<XiaomiBatterySnapshot> ResolveSessionSnapshot(
    const std::optional<XiaomiBatterySnapshot>& status_snapshot,
    const std::optional<XiaomiBatterySnapshot>& device_info_snapshot) {
    if (!status_snapshot.has_value() && !device_info_snapshot.has_value()) {
        return std::nullopt;
    }

    if (!status_snapshot.has_value()) {
        return *device_info_snapshot;
    }

    XiaomiBatterySnapshot merged = *status_snapshot;
    if (device_info_snapshot.has_value()) {
        if (!merged.left.has_value()) {
            merged.left = device_info_snapshot->left;
        }
        if (!merged.right.has_value()) {
            merged.right = device_info_snapshot->right;
        }
        if (!merged.case_level.has_value()) {
            merged.case_level = device_info_snapshot->case_level;
        }
    }
    return merged;
}

void LogMergedSnapshot(bool debug_enabled,
                       XiaomiDebugLogFn debug_log,
                       const XiaomiBatterySnapshot& snapshot) {
    const std::string left_text = snapshot.left.has_value() ? std::to_string(*snapshot.left) : "na";
    const std::string right_text = snapshot.right.has_value() ? std::to_string(*snapshot.right) : "na";
    const std::string case_text = snapshot.case_level.has_value() ? std::to_string(*snapshot.case_level) : "na";
    MaybeDebugLog(debug_enabled,
                  debug_log,
                  "Xiaomi classic fallback: battery merged left=" + left_text +
                      " right=" + right_text + " case=" + case_text);
}

void ObserveModeCandidate(std::uint64_t bluetooth_address,
                          XiaomiModeCacheUpdateFn mode_cache_update,
                          bool debug_enabled,
                          XiaomiDebugLogFn debug_log,
                          const XiaomiMessage& message) {
    const auto mode_code = ParseXiaomiNoiseModeCode(static_cast<std::uint8_t>(message.opcode), message.payload);
    if (!mode_code.has_value()) {
        return;
    }

    const auto submode_code =
        static_cast<std::uint8_t>(message.opcode) == 0xF4U
            ? ParseXiaomiNoiseSubmodeCodeFromF4Payload(message.payload)
            : std::optional<std::uint8_t>{};
    if (mode_cache_update != nullptr) {
        mode_cache_update(bluetooth_address, *mode_code, submode_code);
    }

    MaybeDebugLog(debug_enabled,
                  debug_log,
                  "Xiaomi mode candidate code=" + std::to_string(*mode_code) +
                      " text='" + XiaomiNoiseModeCodeToText(*mode_code) + "'");
}

void LogSessionMessage(bool debug_enabled, XiaomiDebugLogFn debug_log, const XiaomiMessage& message) {
    MaybeDebugLog(debug_enabled, debug_log, FormatXiaomiMessageLine(message, true, "Xiaomi message "));
}

bool ReceiveExact(SOCKET socket_handle, std::uint8_t* output, std::size_t size) {
    std::size_t received = 0;
    while (received < size) {
        const int chunk = recv(socket_handle,
                               reinterpret_cast<char*>(output + received),
                               static_cast<int>(size - received),
                               0);
        if (chunk <= 0) {
            return false;
        }
        received += static_cast<std::size_t>(chunk);
    }
    return true;
}

bool RunZmiRawAuthHandshake(SOCKET socket_handle,
                            bool debug_enabled,
                            XiaomiDebugLogFn debug_log) {
    const auto local_challenge = GenerateRandomChallenge();
    std::vector<std::uint8_t> challenge_request{0x00U};
    challenge_request.insert(challenge_request.end(), local_challenge.begin(), local_challenge.end());
    if (!SendAll(socket_handle, challenge_request)) {
        MaybeDebugLog(debug_enabled, debug_log, "ZMI raw auth: failed to send local challenge");
        return false;
    }

    std::array<std::uint8_t, 17> challenge_response{};
    if (!ReceiveExact(socket_handle, challenge_response.data(), challenge_response.size()) ||
        challenge_response[0] != 0x01U) {
        MaybeDebugLog(debug_enabled, debug_log, "ZMI raw auth: invalid local challenge response");
        return false;
    }
    const auto expected_response = ComputeXiaomiChallengeResponse(local_challenge);
    if (!std::equal(expected_response.begin(), expected_response.end(), challenge_response.begin() + 1)) {
        MaybeDebugLog(debug_enabled, debug_log, "ZMI raw auth: local challenge verification failed");
        return false;
    }

    const std::vector<std::uint8_t> pass_message = {0x02U, 0x70U, 0x61U, 0x73U, 0x73U};
    if (!SendAll(socket_handle, pass_message)) {
        MaybeDebugLog(debug_enabled, debug_log, "ZMI raw auth: failed to send local confirmation");
        return false;
    }

    std::array<std::uint8_t, 17> remote_challenge{};
    if (!ReceiveExact(socket_handle, remote_challenge.data(), remote_challenge.size()) ||
        remote_challenge[0] != 0x00U) {
        MaybeDebugLog(debug_enabled, debug_log, "ZMI raw auth: invalid remote challenge");
        return false;
    }

    std::array<std::uint8_t, 16> remote_challenge_bytes{};
    std::copy_n(remote_challenge.begin() + 1, remote_challenge_bytes.size(), remote_challenge_bytes.begin());
    const auto remote_response = ComputeXiaomiChallengeResponse(remote_challenge_bytes);
    std::vector<std::uint8_t> response_message{0x01U};
    response_message.insert(response_message.end(), remote_response.begin(), remote_response.end());
    if (!SendAll(socket_handle, response_message)) {
        MaybeDebugLog(debug_enabled, debug_log, "ZMI raw auth: failed to send remote challenge response");
        return false;
    }

    std::array<std::uint8_t, 5> remote_confirmation{};
    if (!ReceiveExact(socket_handle, remote_confirmation.data(), remote_confirmation.size()) ||
        !std::equal(pass_message.begin(), pass_message.end(), remote_confirmation.begin())) {
        MaybeDebugLog(debug_enabled, debug_log, "ZMI raw auth: invalid remote confirmation");
        return false;
    }

    MaybeDebugLog(debug_enabled, debug_log, "ZMI raw auth: handshake completed");
    return true;
}

}  // namespace

XiaomiClassicBatterySessionResult RunXiaomiClassicBatterySession(
    SOCKET socket_handle,
    std::uint64_t bluetooth_address,
    bool use_zmi_raw_handshake,
    XiaomiModeCacheUpdateFn mode_cache_update,
    bool debug_enabled,
    XiaomiDebugLogFn debug_log) {
    XiaomiClassicBatterySessionResult result;
    if (socket_handle == INVALID_SOCKET) {
        return result;
    }

    std::uint8_t sequence = 0;
    bool init_requests_sent = false;
    if (use_zmi_raw_handshake) {
        if (!RunZmiRawAuthHandshake(socket_handle, debug_enabled, debug_log)) {
            return result;
        }
        SendXiaomiInfoRequests(socket_handle, &sequence);
        init_requests_sent = true;
        result.auth_start_sent = true;
    } else {
        const auto challenge = GenerateRandomChallenge();
        XiaomiMessage auth_start;
        auth_start.type = XiaomiMessageType::kPhoneRequest;
        auth_start.opcode = XiaomiOpcode::kAuthChallenge;
        auth_start.sequence = sequence++;
        auth_start.payload.push_back(0x01);
        auth_start.payload.insert(auth_start.payload.end(), challenge.begin(), challenge.end());
        if (!SendAll(socket_handle, EncodeXiaomiMessage(auth_start))) {
            MaybeDebugLog(debug_enabled, debug_log, "Xiaomi classic fallback: failed to send auth challenge");
            return result;
        }
        result.auth_start_sent = true;
    }

    std::optional<XiaomiBatterySnapshot> device_info_snapshot;
    std::optional<XiaomiBatterySnapshot> status_snapshot;
    std::optional<std::chrono::steady_clock::time_point> device_info_received_at;
    std::optional<std::chrono::steady_clock::time_point> report_status_seen_at;
    std::vector<std::uint8_t> rx_buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1600);

    while (std::chrono::steady_clock::now() < deadline) {
        if (device_info_snapshot.has_value() && device_info_received_at.has_value() &&
            std::chrono::steady_clock::now() - *device_info_received_at > std::chrono::milliseconds(500)) {
            break;
        }
        if (!status_snapshot.has_value() && report_status_seen_at.has_value() &&
            std::chrono::steady_clock::now() - *report_status_seen_at > std::chrono::milliseconds(900)) {
            break;
        }

        const auto chunk = ReceiveChunk(socket_handle);
        if (!chunk.has_value()) {
            continue;
        }

        const auto messages = AppendAndDecodeXiaomiMessages(&rx_buffer, *chunk);
        for (const auto& message : messages) {
            LogSessionMessage(debug_enabled, debug_log, message);
            ObserveModeCandidate(bluetooth_address, mode_cache_update, debug_enabled, debug_log, message);

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
                    challenge_response.payload.insert(challenge_response.payload.end(),
                                                      response.begin(),
                                                      response.end());
                    SendAll(socket_handle, EncodeXiaomiMessage(challenge_response));
                    continue;
                }
            }

            if (message.opcode == XiaomiOpcode::kAuthConfirm &&
                message.type == XiaomiMessageType::kEarbudsRequest) {
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
                continue;
            }

            if (message.opcode == XiaomiOpcode::kGetDeviceInfo && !message.payload.empty()) {
                const auto extracted = ExtractBatterySnapshotFromXiaomiPayload(
                    message.payload,
                    std::optional<std::uint8_t>{static_cast<std::uint8_t>(0x07U)},
                    debug_enabled,
                    debug_log);
                if (extracted.has_value()) {
                    device_info_snapshot = extracted;
                    device_info_received_at = std::chrono::steady_clock::now();
                }
                continue;
            }

            if (message.opcode == XiaomiOpcode::kReportStatus && !message.payload.empty()) {
                report_status_seen_at = std::chrono::steady_clock::now();
                SendXiaomiReportStatusAck(socket_handle, message);

                const auto extracted = ExtractBatterySnapshotFromXiaomiPayload(
                    message.payload,
                    std::optional<std::uint8_t>{static_cast<std::uint8_t>(0x00U)},
                    debug_enabled,
                    debug_log);
                if (extracted.has_value()) {
                    status_snapshot = extracted;
                    break;
                }
                continue;
            }

        }

        if (status_snapshot.has_value()) {
            break;
        }
    }

    const auto merged_snapshot = ResolveSessionSnapshot(status_snapshot, device_info_snapshot);
    if (!merged_snapshot.has_value()) {
        return result;
    }

    result.readings = BuildXiaomiBatteryReadings(*merged_snapshot);
    LogMergedSnapshot(debug_enabled, debug_log, *merged_snapshot);
    return result;
}

}  // namespace battery_monitor

