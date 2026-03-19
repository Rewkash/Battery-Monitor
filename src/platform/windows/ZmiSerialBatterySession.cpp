#include "platform/windows/ZmiSerialBatterySession.h"

#include "platform/windows/BluetoothSocketUtils.h"
#include "platform/windows/XiaomiBatteryReadings.h"
#include "platform/windows/XiaomiControlSession.h"
#include "platform/windows/ZmiBatteryCodec.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace battery_monitor {

namespace {

void MaybeDebugLog(bool debug_enabled, ZmiSerialDebugLogFn debug_log, const std::string& message) {
    if (debug_enabled && debug_log != nullptr) {
        debug_log(message);
    }
}

std::vector<std::vector<std::uint8_t>> BuildZmiBinaryProbes() {
    return {
        {0xFE, 0xDC, 0xBA, 0xC3, 0x02, 0x00, 0x05, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x42},
        {0xFE, 0xDC, 0xBA, 0xC3, 0x09, 0x00, 0x05, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x42},
        {0xFE, 0xDC, 0xBA, 0xC3, 0x02, 0x00, 0x05, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x42},
        {0xFE, 0xDC, 0xBA, 0xC3, 0x09, 0x00, 0x05, 0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x42},
        {0xFE, 0xDC, 0xBA, 0xC3, 0xF1, 0x00, 0x07, 0x03, 0x5A, 0x4D, 0xEA, 0x02, 0x81, 0x00, 0xEF, 0x5E},
        {0xFE, 0xDC, 0xBA, 0xC3, 0xF1, 0x00, 0x07, 0x04, 0x5A, 0x4D, 0xEA, 0x02, 0x81, 0x00, 0xEF, 0x5E},
        {0xFE, 0xDC, 0xBA, 0xC3, 0xF1, 0x00, 0x07, 0x05, 0x5A, 0x4D, 0xEA, 0x02, 0x81, 0x00, 0xEF, 0x5E},
    };
}

constexpr std::array<const char*, 33> kZmiTextProbes = {
    "AT\r",
    "AT+XAPL=ABCD-1234-0100,7\r",
    "AT+CIND?\r",
    "AT+CIND=?\r",
    "AT+BRSF=20\r",
    "AT+IPHONEACCEV?\r",
    "AT+XEVENT?\r",
    "AT+XEVENT=BATTERY?\r",
    "AT+XEVENT=BATTERY\r",
    "AT+XEVENT=BATTERY,?\r",
    "AT+XEVENT=BATTERY,GET\r",
    "AT+XEVENT=BATTERY,0\r",
    "AT+XEVENT=BATTERY,1\r",
    "AT+XEVENT=GETBATTERY\r",
    "AT+XEVENT=GETBAT\r",
    "AT+XEVENT=STATUS?\r",
    "AT+BIEV?\r",
    "AT+CMER=3,0,0,1\r",
    "AT+BATT?\r",
    "AT+BATTERY?\r",
    "AT+QBAT?\r",
    "AT+BLEGETBAT?\r",
    "AT+GETBAT?\r",
    "AT+MBATT?\r",
    "AT+MIBAT?\r",
    "AT+BAT?\r",
    "AT+EARBAT?\r",
    "AT+PODBAT?\r",
    "AT+XIAOMI?\r",
    "AT+XMINFO?\r",
    "AT+STATUS?\r",
    "AT+VGS?\r",
    "AT+VGM?\r",
};

}  // namespace

std::vector<BatteryReading> TryReadZmiSerialBatteryFromSocket(
    SOCKET socket_handle,
    ZmiReplyToHfpAgCommandFn reply_to_hfp_ag_command,
    ZmiParseAtBatteryPercentFn parse_at_battery_percent,
    bool debug_enabled,
    ZmiSerialDebugLogFn debug_log,
    int observe_ms) {
    std::vector<BatteryReading> readings;
    if (socket_handle == INVALID_SOCKET) {
        return readings;
    }

    const int recv_timeout_ms = 75;
    const int send_timeout_ms = 120;
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&recv_timeout_ms), sizeof(recv_timeout_ms));
    setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&send_timeout_ms), sizeof(send_timeout_ms));

    const auto probes = BuildZmiBinaryProbes();
    std::vector<std::uint8_t> rolling_buffer;
    rolling_buffer.reserve(4096);
    std::vector<std::uint8_t> message_buffer;
    message_buffer.reserve(4096);
    std::optional<std::uint8_t> fallback_main;

    auto process_chunk = [&](const std::vector<std::uint8_t>& bytes, const char* source_tag) -> bool {
        if (bytes.empty()) {
            return false;
        }

        MaybeDebugLog(debug_enabled,
                      debug_log,
                      std::string("ZMI serial rx source=") + source_tag + " " + FormatXiaomiChunkLine(bytes));

        rolling_buffer.insert(rolling_buffer.end(), bytes.begin(), bytes.end());
        if (rolling_buffer.size() > 8192U) {
            rolling_buffer.erase(rolling_buffer.begin(),
                                 rolling_buffer.begin() + static_cast<std::ptrdiff_t>(rolling_buffer.size() - 8192U));
        }

        if (const auto snapshot = ExtractZmiSerialPatternSnapshot(bytes); snapshot.has_value()) {
            readings = BuildXiaomiBatteryReadings(*snapshot);
            if (HasUsefulXiaomiTwsReadings(readings)) {
                MaybeDebugLog(debug_enabled, debug_log, "ZMI serial fallback: extracted triplet from pattern (chunk)");
                return true;
            }
        }
        if (const auto snapshot = ExtractZmiSerialPatternSnapshot(rolling_buffer); snapshot.has_value()) {
            readings = BuildXiaomiBatteryReadings(*snapshot);
            if (HasUsefulXiaomiTwsReadings(readings)) {
                MaybeDebugLog(debug_enabled, debug_log, "ZMI serial fallback: extracted triplet from pattern (rolling)");
                return true;
            }
        }

        if (const auto snapshot = ExtractPreferredXiaomiBatterySnapshot(bytes, debug_enabled, debug_log);
            snapshot.has_value()) {
            readings = BuildXiaomiBatteryReadings(*snapshot);
            if (HasUsefulXiaomiTwsReadings(readings)) {
                MaybeDebugLog(debug_enabled, debug_log, "ZMI serial fallback: extracted triplet from raw payload");
                return true;
            }
        }

        const auto messages = AppendAndDecodeXiaomiMessages(&message_buffer, bytes);
        for (const auto& message : messages) {
            MaybeDebugLog(debug_enabled, debug_log, FormatXiaomiMessageLine(message, true, "ZMI serial message "));
            if (message.payload.empty()) {
                continue;
            }

            const auto extracted =
                ExtractPreferredXiaomiBatterySnapshot(message.payload, debug_enabled, debug_log);
            if (extracted.has_value()) {
                const auto candidate = BuildXiaomiBatteryReadings(*extracted);
                if (XiaomiReadingsRichnessScore(candidate) > XiaomiReadingsRichnessScore(readings)) {
                    readings = candidate;
                }
                if (HasUsefulXiaomiTwsReadings(readings)) {
                    MaybeDebugLog(debug_enabled, debug_log,
                                  "ZMI serial fallback: extracted triplet from Xiaomi message payload");
                    return true;
                }
            }
        }

        const std::string chunk_text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::size_t line_start = 0;
        while (line_start < chunk_text.size()) {
            const auto line_end = chunk_text.find_first_of("\r\n", line_start);
            std::string line;
            if (line_end == std::string::npos) {
                line = chunk_text.substr(line_start);
                line_start = chunk_text.size();
            } else {
                line = chunk_text.substr(line_start, line_end - line_start);
                line_start = line_end + 1U;
            }
            if (line.empty()) {
                continue;
            }

            if (reply_to_hfp_ag_command != nullptr) {
                reply_to_hfp_ag_command(socket_handle, line);
            }
            if (parse_at_battery_percent != nullptr) {
                if (const auto parsed = parse_at_battery_percent(line); parsed.has_value()) {
                    fallback_main = parsed;
                }
            }
            if (const auto snapshot = ExtractZmiSerialTextSnapshot(line); snapshot.has_value()) {
                const auto candidate = BuildXiaomiBatteryReadings(*snapshot);
                if (XiaomiReadingsRichnessScore(candidate) > XiaomiReadingsRichnessScore(readings)) {
                    readings = candidate;
                }
                if (HasUsefulXiaomiTwsReadings(readings)) {
                    MaybeDebugLog(debug_enabled, debug_log, "ZMI serial fallback: extracted triplet from text line");
                    return true;
                }
            }
        }
        if (const auto snapshot = ExtractZmiSerialTextSnapshot(chunk_text); snapshot.has_value()) {
            const auto candidate = BuildXiaomiBatteryReadings(*snapshot);
            if (XiaomiReadingsRichnessScore(candidate) > XiaomiReadingsRichnessScore(readings)) {
                readings = candidate;
            }
            if (HasUsefulXiaomiTwsReadings(readings)) {
                MaybeDebugLog(debug_enabled, debug_log, "ZMI serial fallback: extracted triplet from text chunk");
                return true;
            }
        }

        return false;
    };

    auto receive_attempts = [&](int attempts, int wait_ms, const char* source_tag) {
        for (int attempt = 0; attempt < attempts; ++attempt) {
            if (wait_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
            }
            const auto chunk = ReceiveChunk(socket_handle);
            if (!chunk.has_value() || chunk->empty()) {
                continue;
            }
            if (process_chunk(*chunk, source_tag)) {
                return true;
            }
        }
        return false;
    };

    if (receive_attempts(2, 25, "warmup")) {
        return readings;
    }

    for (const auto& probe : probes) {
        if (!SendAll(socket_handle, probe)) {
            continue;
        }
        if (receive_attempts(2, 35, "binary-probe")) {
            return readings;
        }
    }

    auto send_text_probe = [&](const char* probe) {
        const std::size_t command_length = std::strlen(probe);
        if (send(socket_handle, probe, static_cast<int>(command_length), 0) <= 0) {
            return false;
        }
        if (command_length > 0U && probe[command_length - 1U] == '\r') {
            std::string probe_crlf(probe, command_length);
            probe_crlf.push_back('\n');
            send(socket_handle, probe_crlf.data(), static_cast<int>(probe_crlf.size()), 0);
        }
        return true;
    };

    for (const auto* probe : kZmiTextProbes) {
        if (!send_text_probe(probe)) {
            continue;
        }
        if (receive_attempts(1, 30, "text-probe")) {
            return readings;
        }
    }

    if (observe_ms > 0) {
        const auto observe_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(observe_ms);
        auto next_active_probe_at = std::chrono::steady_clock::now();
        std::size_t next_binary_probe = 0;
        std::size_t next_text_probe = 0;
        while (std::chrono::steady_clock::now() < observe_deadline) {
            const auto chunk = ReceiveChunk(socket_handle);
            if (chunk.has_value() && !chunk->empty()) {
                if (process_chunk(*chunk, "observe")) {
                    return readings;
                }
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= next_active_probe_at) {
                if (!probes.empty()) {
                    const auto& probe = probes[next_binary_probe % probes.size()];
                    ++next_binary_probe;
                    SendAll(socket_handle, probe);
                }
                if (!kZmiTextProbes.empty()) {
                    send_text_probe(kZmiTextProbes[next_text_probe % kZmiTextProbes.size()]);
                    ++next_text_probe;
                }
                next_active_probe_at = now + std::chrono::milliseconds(650);
            }

            if (receive_attempts(1, 20, "observe-probe")) {
                return readings;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(35));
        }
    }

    if (!readings.empty()) {
        return readings;
    }
    if (fallback_main.has_value()) {
        readings.push_back(BatteryReading{"main", *fallback_main});
    }
    return readings;
}

}  // namespace battery_monitor
