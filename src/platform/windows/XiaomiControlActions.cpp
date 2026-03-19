#include "platform/windows/XiaomiControlActions.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>

#include "platform/windows/BluetoothSocketUtils.h"
#include "platform/windows/XiaomiBatteryCodec.h"
#include "platform/windows/XiaomiControlSession.h"
#include "platform/windows/XiaomiModeCache.h"
#include "platform/windows/XiaomiNoiseModeCodec.h"
#include "platform/windows/XiaomiProtocol.h"

namespace battery_monitor {

namespace {

struct XiaomiModePayload {
    std::uint8_t mode_value = 0;
    std::uint8_t f4_tail_value = 0;
};

struct XiaomiCandidateCommand {
    int id = 0;
    std::uint8_t opcode = 0;
    std::vector<std::uint8_t> payload;
    const char* label = "";
};

std::string ToLowerAscii(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

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

bool OpenControlConnection(std::uint64_t address,
                           XiaomiControlConnection* connection,
                           const XiaomiControlActionContext& context,
                           const char* socket_failed_message,
                           bool authenticate,
                           bool print_output) {
    if (connection == nullptr) {
        return false;
    }

    const auto open_status = connection->OpenSocket(address);
    if (open_status == XiaomiControlSocketStatus::kWsaStartupFailed) {
        if (print_output) {
            std::cout << "WSAStartup failed.\n";
        }
        return false;
    }
    if (open_status == XiaomiControlSocketStatus::kSocketOpenFailed) {
        if (print_output) {
            std::cout << socket_failed_message << "\n";
        }
        return false;
    }

    if (print_output) {
        std::cout << "Connected via " << connection->connected_path() << "\n";
    }

    if (authenticate && !connection->Authenticate(context.debug_enabled, context.debug_log)) {
        if (print_output) {
            std::cout << "Auth handshake failed.\n";
        }
        return false;
    }

    return true;
}

void ObservePrintedTraffic(SOCKET socket_handle,
                           std::chrono::steady_clock::time_point deadline,
                           bool acknowledge_report_status) {
    std::vector<std::uint8_t> rx_buffer;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto chunk = ReceiveChunk(socket_handle);
        if (!chunk.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        std::cout << FormatXiaomiChunkLine(*chunk) << "\n";
        const auto messages = AppendAndDecodeXiaomiMessages(&rx_buffer, *chunk);
        for (const auto& response : messages) {
            std::cout << FormatXiaomiMessageLine(response, true) << "\n";

            if (acknowledge_report_status && IsXiaomiReportStatusNotification(response)) {
                SendXiaomiReportStatusAck(socket_handle, response);
            }
        }
    }
}

std::optional<XiaomiModePayload> TryParseRequestedMode(const std::string& mode) {
    const std::string normalized_mode = ToLowerAscii(mode);
    if (normalized_mode == "off" || normalized_mode == "disable" || normalized_mode == "disabled") {
        return XiaomiModePayload{0x00, 0x00};
    }
    if (normalized_mode == "anc" || normalized_mode == "noise" || normalized_mode == "noise-canceling") {
        return XiaomiModePayload{0x01, 0x02};
    }
    if (normalized_mode == "transparency" || normalized_mode == "transparent") {
        return XiaomiModePayload{0x02, 0x01};
    }
    return std::nullopt;
}

const std::vector<XiaomiCandidateCommand>& XiaomiControlCandidates() {
    static const std::vector<XiaomiCandidateCommand> candidates = {
        {1, 0x08, {0x02, 0x04, 0x00}, "c1-08 off"},
        {2, 0x08, {0x02, 0x04, 0x01}, "c1-08 anc"},
        {3, 0x08, {0x02, 0x04, 0x02}, "c1-08 transparency"},
        {4, 0xF4, {0x04, 0x00, 0x0B, 0x00, 0x00}, "f4 style off"},
        {5, 0xF4, {0x04, 0x00, 0x0B, 0x01, 0x00}, "f4 style anc"},
        {6, 0xF4, {0x04, 0x00, 0x0B, 0x02, 0x01}, "f4 style transparency"},
        {7, 0xF4, {0x04, 0x00, 0x0B, 0x01, 0x01}, "f4 alt anc"},
        {8, 0xF4, {0x04, 0x00, 0x0B, 0x02, 0x00}, "f4 alt transparency"},
        {9, 0xF4, {0x04, 0x00, 0x0B, 0x00, 0x01}, "f4 alt off"},
        {10, 0xF4, {0x04, 0x00, 0x0B, 0x00, 0x00}, "f4 off as response"},
        {11, 0xF4, {0x04, 0x00, 0x0B, 0x01, 0x00}, "f4 anc as response"},
        {12, 0xF4, {0x04, 0x00, 0x0B, 0x02, 0x01}, "f4 transparency as response"},
        {13, 0xF4, {0x04, 0x00, 0x0B, 0x00, 0x00}, "f4 off as earbuds-request"},
        {14, 0xF4, {0x04, 0x00, 0x0B, 0x01, 0x00}, "f4 anc as earbuds-request"},
        {15, 0xF4, {0x04, 0x00, 0x0B, 0x02, 0x01}, "f4 transparency as earbuds-request"},
    };
    return candidates;
}

}  // namespace

bool ProbeXiaomiNoiseControlForTarget(const ResolvedBluetoothTarget& target,
                                      const XiaomiControlActionContext& context) {
    std::cout << "Probing device: " << target.first << " address=" << target.second << "\n";
    std::cout << "Watch the earbuds state. Each candidate waits about 2 seconds.\n";
    std::cout << "If a mode changes, note the candidate number and the observed mode.\n";

    XiaomiControlConnection connection;
    if (!OpenControlConnection(
            target.second, &connection, context, "Failed to open Xiaomi control socket.", true, true)) {
        return false;
    }

    const auto commands = BuildXiaomiNoiseProbeCommands();
    std::vector<std::uint8_t> rx_buffer;
    for (std::size_t index = 0; index < commands.size(); ++index) {
        const auto& command = commands[index];
        XiaomiMessage message;
        message.type = command.type;
        message.opcode = static_cast<XiaomiOpcode>(command.opcode);
        message.sequence = connection.sequence()++;
        message.payload = command.payload;

        const auto bytes = EncodeXiaomiMessage(message);
        std::cout << "[" << (index + 1) << "/" << commands.size() << "] "
                  << command.label
                  << " opcode=" << ByteToHex(command.opcode)
                  << " payload=" << BytesToHex(command.payload) << "\n";

        if (!SendAll(connection.socket_handle(), bytes)) {
            std::cout << "Send failed for candidate " << (index + 1) << "\n";
            continue;
        }

        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(command.pause_ms);
        while (std::chrono::steady_clock::now() < until) {
            const auto chunk = ReceiveChunk(connection.socket_handle());
            if (!chunk.has_value()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            const auto messages = AppendAndDecodeXiaomiMessages(&rx_buffer, *chunk);
            for (const auto& response : messages) {
                std::cout << FormatXiaomiMessageLine(response, false, "  rx ") << "\n";
            }
        }
    }

    std::cout << "Probe finished.\n";
    return true;
}

bool ObserveXiaomiControlSessionForTarget(const ResolvedBluetoothTarget& target,
                                          int duration_seconds,
                                          const XiaomiControlActionContext& context) {
    std::cout << "Observing device: " << target.first << " address=" << target.second << "\n";
    std::cout << "Observation window: " << duration_seconds << " seconds.\n";
    std::cout << "Now switch ANC/transparency/off on the earbuds or in the phone app.\n";

    XiaomiControlConnection connection;
    if (!OpenControlConnection(
            target.second, &connection, context, "Failed to open Xiaomi control socket.", true, true)) {
        return false;
    }

    SendXiaomiInfoRequests(connection.socket_handle(), &connection.sequence(), true);
    ObservePrintedTraffic(
        connection.socket_handle(),
        std::chrono::steady_clock::now() + std::chrono::seconds(duration_seconds),
        true);

    std::cout << "Observation finished.\n";
    return true;
}

bool ObserveZmiSerialSessionForTarget(const ResolvedBluetoothTarget& target,
                                      int duration_seconds,
                                      const XiaomiControlActionContext& context) {
    std::cout << "Observing ZMI serial device: " << target.first << " address=" << target.second << "\n";
    std::cout << "Observation window: " << duration_seconds << " seconds.\n";

    XiaomiControlConnection connection;
    if (!OpenControlConnection(
            target.second, &connection, context, "Failed to open ZMI serial/control socket.", false, true)) {
        return false;
    }

    std::vector<std::vector<std::uint8_t>> probes;
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0x02, 0x00, 0x05, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x42});
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0x09, 0x00, 0x05, 0x01,
        0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x42});
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0x02, 0x00, 0x05, 0x02,
        0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x42});
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0x09, 0x00, 0x05, 0x08,
        0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x42});
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0xF1, 0x00, 0x07, 0x03,
        0x5A, 0x4D, 0xEA, 0x02, 0x81, 0x00, 0xEF, 0x5E});
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0xF1, 0x00, 0x07, 0x04,
        0x5A, 0x4D, 0xEA, 0x02, 0x81, 0x00, 0xEF, 0x5E});
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0xF1, 0x00, 0x07, 0x05,
        0x5A, 0x4D, 0xEA, 0x02, 0x81, 0x00, 0xEF, 0x5E});

    constexpr std::array<const char*, 9> kTextProbes = {
        "AT\r",
        "AT+XAPL=ABCD-1234-0100,7\r",
        "AT+IPHONEACCEV?\r",
        "AT+XEVENT?\r",
        "AT+XEVENT=BATTERY?\r",
        "AT+XEVENT=GETBATTERY\r",
        "AT+BATT?\r",
        "AT+BATTERY?\r",
        "AT+STATUS?\r",
    };

    int probe_index = 1;
    for (const auto& probe : probes) {
        std::cout << "tx probe[" << probe_index++ << "]: " << BytesToHex(probe) << "\n";
        SendAll(connection.socket_handle(), probe);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    for (const char* probe : kTextProbes) {
        const std::string line = probe;
        send(connection.socket_handle(), line.data(), static_cast<int>(line.size()), 0);
        std::cout << "tx text: " << line;
        if (line.empty() || line.back() != '\n') {
            std::cout << "\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }

    std::vector<std::uint8_t> rx_buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto chunk = ReceiveChunk(connection.socket_handle());
        if (!chunk.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            continue;
        }

        std::cout << FormatXiaomiChunkLine(*chunk) << "\n";

        if (const auto raw_extracted =
                ExtractPreferredXiaomiBatterySnapshot(*chunk, context.debug_enabled, context.debug_log);
            raw_extracted.has_value()) {
            const auto left_text = raw_extracted->left.has_value() ? std::to_string(*raw_extracted->left) : "na";
            const auto right_text = raw_extracted->right.has_value() ? std::to_string(*raw_extracted->right) : "na";
            const auto case_text =
                raw_extracted->case_level.has_value() ? std::to_string(*raw_extracted->case_level) : "na";
            std::cout << "raw battery candidate left=" << left_text
                      << " right=" << right_text
                      << " case=" << case_text << "\n";
        }

        const auto messages = AppendAndDecodeXiaomiMessages(&rx_buffer, *chunk);
        for (const auto& response : messages) {
            std::cout << FormatXiaomiMessageLine(response, true) << "\n";

            if (const auto extracted =
                    ExtractPreferredXiaomiBatterySnapshot(response.payload, context.debug_enabled, context.debug_log);
                extracted.has_value()) {
                const auto left_text = extracted->left.has_value() ? std::to_string(*extracted->left) : "na";
                const auto right_text = extracted->right.has_value() ? std::to_string(*extracted->right) : "na";
                const auto case_text =
                    extracted->case_level.has_value() ? std::to_string(*extracted->case_level) : "na";
                std::cout << "payload battery candidate left=" << left_text
                          << " right=" << right_text
                          << " case=" << case_text << "\n";
            }
        }
    }

    std::cout << "Observation finished.\n";
    return true;
}

bool SetXiaomiNoiseModeForTarget(const ResolvedBluetoothTarget& target,
                                 const std::string& mode,
                                 const XiaomiControlActionContext& context) {
    const auto mode_payload = TryParseRequestedMode(mode);
    if (!mode_payload.has_value()) {
        std::cout << "Unknown mode. Use one of: off, anc, transparency\n";
        return false;
    }

    const std::string normalized_mode = ToLowerAscii(mode);
    std::cout << "Setting mode on device: " << target.first << " address=" << target.second << "\n";
    std::cout << "Requested mode: " << normalized_mode << " (experimental)\n";

    XiaomiControlConnection connection;
    if (!OpenControlConnection(
            target.second, &connection, context, "Failed to open Xiaomi control socket.", true, true)) {
        return false;
    }

    const auto send_command = [&](std::uint8_t raw_type,
                                  std::uint8_t raw_opcode,
                                  std::initializer_list<std::uint8_t> payload) -> bool {
        XiaomiMessage message;
        message.type = static_cast<XiaomiMessageType>(raw_type);
        message.opcode = static_cast<XiaomiOpcode>(raw_opcode);
        message.sequence = connection.sequence()++;
        message.payload.assign(payload.begin(), payload.end());

        std::cout << "Sending type=" << ByteToHex(raw_type)
                  << " opcode=" << ByteToHex(raw_opcode)
                  << " payload=" << BytesToHex(message.payload) << "\n";
        return SendAll(connection.socket_handle(), EncodeXiaomiMessage(message));
    };

    if (!send_command(0x01U, 0x0EU, {0x02, 0x04, mode_payload->mode_value})) {
        std::cout << "Send failed for opcode 0x0E.\n";
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    if (!send_command(0x01U,
                      0xF4U,
                      {0x04, 0x00, 0x0B, mode_payload->mode_value, mode_payload->f4_tail_value})) {
        std::cout << "Send failed for opcode 0xF4.\n";
        return false;
    }

    ObservePrintedTraffic(
        connection.socket_handle(),
        std::chrono::steady_clock::now() + std::chrono::milliseconds(2500),
        true);

    std::cout << "Command finished.\n";
    return true;
}

bool SendXiaomiControlCandidateForTarget(const ResolvedBluetoothTarget& target,
                                         int candidate_id,
                                         const XiaomiControlActionContext& context) {
    const auto& candidates = XiaomiControlCandidates();
    const auto candidate_it = std::find_if(candidates.begin(), candidates.end(),
                                           [candidate_id](const XiaomiCandidateCommand& candidate) {
                                               return candidate.id == candidate_id;
                                           });
    if (candidate_it == candidates.end()) {
        std::cout << "Unknown candidate. Use 1..9\n";
        return false;
    }

    std::cout << "Testing candidate #" << candidate_it->id << " on device: " << target.first << "\n";
    std::cout << "Label: " << candidate_it->label << "\n";
    std::cout << "Opcode=" << ByteToHex(candidate_it->opcode)
              << " payload=" << BytesToHex(candidate_it->payload) << "\n";

    XiaomiControlConnection connection;
    if (!OpenControlConnection(
            target.second, &connection, context, "Failed to open Xiaomi control socket.", true, true)) {
        return false;
    }

    XiaomiMessage message;
    if (candidate_it->id <= 3) {
        message.type = static_cast<XiaomiMessageType>(0xC1U);
    } else if (candidate_it->id >= 13) {
        message.type = XiaomiMessageType::kEarbudsRequest;
    } else if (candidate_it->id >= 10) {
        message.type = XiaomiMessageType::kResponse;
    } else {
        message.type = XiaomiMessageType::kPhoneRequest;
    }
    message.opcode = static_cast<XiaomiOpcode>(candidate_it->opcode);
    message.sequence = connection.sequence()++;
    message.payload = candidate_it->payload;

    if (!SendAll(connection.socket_handle(), EncodeXiaomiMessage(message))) {
        std::cout << "Send failed.\n";
        return false;
    }

    ObservePrintedTraffic(
        connection.socket_handle(),
        std::chrono::steady_clock::now() + std::chrono::milliseconds(2500),
        true);

    std::cout << "Candidate finished.\n";
    return true;
}

bool SetXiaomiNoiseSubmodeForTarget(const ResolvedBluetoothTarget& target,
                                    const std::string& family,
                                    int submode,
                                    const XiaomiControlActionContext& context) {
    const std::string normalized_family = ToLowerAscii(family);
    std::uint8_t family_code = 0;
    int max_submode = 0;
    if (normalized_family == "transparency" || normalized_family == "transparent") {
        family_code = 0x02;
        max_submode = 1;
    } else if (normalized_family == "anc") {
        family_code = 0x01;
        max_submode = 3;
    } else {
        std::cout << "Unknown family. Use anc or transparency\n";
        return false;
    }

    if (submode < 0 || submode > max_submode) {
        std::cout << "Submode out of range.\n";
        return false;
    }

    XiaomiControlConnection connection;
    if (!OpenControlConnection(
            target.second, &connection, context, "Failed to open Xiaomi control socket.", true, false)) {
        return false;
    }

    XiaomiMessage message;
    message.type = static_cast<XiaomiMessageType>(0xC1U);
    message.opcode = static_cast<XiaomiOpcode>(0xF2U);
    message.sequence = connection.sequence()++;
    message.payload = {0x04, 0x00, 0x0B, family_code, static_cast<std::uint8_t>(submode)};
    std::cout << "Sending submode family=" << normalized_family
              << " submode=" << submode
              << " payload=" << BytesToHex(message.payload) << "\n";
    const bool sent = SendAll(connection.socket_handle(), EncodeXiaomiMessage(message));

    ObservePrintedTraffic(
        connection.socket_handle(),
        std::chrono::steady_clock::now() + std::chrono::milliseconds(2500),
        false);

    if (sent) {
        PutXiaomiModeCacheEntry(
            target.second, family_code == 0x01U ? 0x01U : 0x02U, static_cast<std::uint8_t>(submode));
    }
    return sent;
}

bool SetNoiseControlModeForAddress(std::uint64_t address,
                                   NoiseControlMode mode,
                                   const XiaomiControlActionContext& context) {
    XiaomiControlConnection connection;
    if (!OpenControlConnection(address, &connection, context, "", true, false)) {
        return false;
    }

    std::uint8_t mode_value = 0;
    switch (mode) {
        case NoiseControlMode::Off:
            mode_value = 0x00;
            break;
        case NoiseControlMode::Anc:
            mode_value = 0x01;
            break;
        case NoiseControlMode::Transparency:
            mode_value = 0x02;
            break;
    }

    XiaomiMessage message;
    message.type = static_cast<XiaomiMessageType>(0xC1U);
    message.opcode = static_cast<XiaomiOpcode>(0x08U);
    message.sequence = connection.sequence()++;
    message.payload = {0x02, 0x04, mode_value};
    if (!SendAll(connection.socket_handle(), EncodeXiaomiMessage(message))) {
        return false;
    }

    bool observed_confirmation = false;
    std::vector<std::uint8_t> rx_buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1800);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto chunk = ReceiveChunk(connection.socket_handle());
        if (!chunk.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            continue;
        }
        const auto messages = AppendAndDecodeXiaomiMessages(&rx_buffer, *chunk);
        for (const auto& response : messages) {
            const auto parsed_mode =
                ParseXiaomiNoiseModeCode(static_cast<std::uint8_t>(response.opcode), response.payload);
            if (!parsed_mode.has_value()) {
                continue;
            }

            const auto parsed_submode =
                static_cast<std::uint8_t>(response.opcode) == 0xF4U
                    ? ParseXiaomiNoiseSubmodeCodeFromF4Payload(response.payload)
                    : std::optional<std::uint8_t>{};
            PutXiaomiModeCacheEntry(address, *parsed_mode, parsed_submode);
            if (*parsed_mode == mode_value) {
                observed_confirmation = true;
            }
        }
    }

    return observed_confirmation;
}

}  // namespace battery_monitor
