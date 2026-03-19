#include "platform/windows/HfpBatterySession.h"

#include "platform/windows/BluetoothSocketUtils.h"
#include "platform/windows/XiaomiBatteryCodec.h"
#include "platform/windows/ZmiBatteryCodec.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace battery_monitor {

namespace {

void MaybeDebugLog(bool debug_enabled, HfpDebugLogFn debug_log, const std::string& message) {
    if (debug_enabled && debug_log != nullptr) {
        debug_log(message);
    }
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::vector<int> ExtractIntegersFromText(const std::string& text) {
    std::vector<int> numbers;
    int current = -1;
    for (const char ch : text) {
        if (ch >= '0' && ch <= '9') {
            if (current < 0) {
                current = 0;
            }
            current = (current * 10) + static_cast<int>(ch - '0');
            continue;
        }

        if (current >= 0) {
            numbers.push_back(current);
            current = -1;
        }
    }

    if (current >= 0) {
        numbers.push_back(current);
    }

    return numbers;
}

std::optional<std::uint8_t> NormalizeAtBatteryPercent(int value) {
    if (value < 0) {
        return std::nullopt;
    }
    if (value <= 9) {
        return static_cast<std::uint8_t>(std::min(100, value * 10));
    }
    if (value <= 100) {
        return static_cast<std::uint8_t>(value);
    }
    return std::nullopt;
}

std::optional<std::uint8_t> NormalizeCindBatteryPercent(int value) {
    if (value >= 0 && value <= 5) {
        return static_cast<std::uint8_t>(value * 20);
    }
    return NormalizeAtBatteryPercent(value);
}

std::optional<std::uint8_t> TryParseHfpCindBatteryPercent(const std::string& response_text) {
    std::vector<std::string> indicator_names;
    std::vector<int> indicator_values;

    std::istringstream reader(response_text);
    std::string line;
    while (std::getline(reader, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string lowered = ToLowerAscii(line);
        const auto cind_pos = lowered.find("+cind:");
        if (cind_pos == std::string::npos) {
            continue;
        }

        const auto payload = lowered.substr(cind_pos + 6U);
        if (payload.find('"') != std::string::npos) {
            std::size_t cursor = 0;
            while (cursor < payload.size()) {
                const auto open = payload.find('"', cursor);
                if (open == std::string::npos) {
                    break;
                }
                const auto close = payload.find('"', open + 1U);
                if (close == std::string::npos) {
                    break;
                }
                indicator_names.push_back(payload.substr(open + 1U, close - open - 1U));
                cursor = close + 1U;
            }
            continue;
        }

        indicator_values = ExtractIntegersFromText(payload);
    }

    if (indicator_names.empty() || indicator_values.empty()) {
        return std::nullopt;
    }

    for (std::size_t index = 0; index < indicator_names.size(); ++index) {
        const auto& indicator = indicator_names[index];
        if (indicator != "battchg" && indicator.find("battery") == std::string::npos &&
            indicator.find("batt") == std::string::npos) {
            continue;
        }
        if (index >= indicator_values.size()) {
            continue;
        }
        return NormalizeCindBatteryPercent(indicator_values[index]);
    }

    return std::nullopt;
}

}  // namespace

void ReplyToHfpAgCommand(SOCKET socket_handle, const std::string& line) {
    if (socket_handle == INVALID_SOCKET || line.empty()) {
        return;
    }

    auto send_reply = [&](const std::string& reply) {
        if (reply.empty()) {
            return;
        }
        send(socket_handle, reply.data(), static_cast<int>(reply.size()), 0);
    };

    const std::string lowered = ToLowerAscii(line);
    if (lowered.rfind("at+brsf=", 0) == 0U) {
        send_reply("\r\n+BRSF: 1024\r\n\r\nOK\r\n");
        send_reply("\r\n+XAPL=ABCD-1234-0100,7\r\n");
        return;
    }
    if (lowered.rfind("at+cind=?", 0) == 0U) {
        send_reply("\r\n+CIND: (\"service\",(0,1)),(\"call\",(0,1)),(\"callsetup\",(0-3)),"
                   "(\"callheld\",(0-2)),(\"battchg\",(0-5))\r\n\r\nOK\r\n");
        return;
    }
    if (lowered.rfind("at+cind?", 0) == 0U) {
        send_reply("\r\n+CIND: 1,0,0,0,5\r\n\r\nOK\r\n");
        return;
    }
    if (lowered.rfind("at+cmer=", 0) == 0U ||
        lowered.rfind("at+bac=", 0) == 0U ||
        lowered.rfind("at+xapl=", 0) == 0U) {
        send_reply("\r\nOK\r\n");
        if (lowered.rfind("at+cmer=", 0) == 0U) {
            send_reply("AT+XEVENT?\r");
            send_reply("AT+XEVENT=BATTERY?\r");
            send_reply("AT+BATT?\r");
            send_reply("AT+BATTERY?\r");
        }
        return;
    }
    if (lowered.rfind("at+iphoneaccev=", 0) == 0U ||
        lowered.rfind("at+xevent", 0) == 0U ||
        lowered.rfind("at+biev", 0) == 0U) {
        send_reply("\r\nOK\r\n");
        send_reply("AT+XEVENT=BATTERY?\r");
        return;
    }
    if (lowered.rfind("at+aplsiri?", 0) == 0U) {
        send_reply("\r\n+APLSIRI: 1\r\n\r\nOK\r\n");
        return;
    }
    if (lowered.rfind("at+", 0) == 0U || lowered == "at") {
        send_reply("\r\nOK\r\n");
    }
}

std::optional<std::uint8_t> ParseAtBatteryPercentFromLine(const std::string& line) {
    const std::string lowered = ToLowerAscii(line);
    const auto numbers = ExtractIntegersFromText(lowered);
    if (numbers.empty()) {
        return std::nullopt;
    }

    if (lowered.find("+iphoneaccev") != std::string::npos) {
        if (numbers.size() >= 2U) {
            return NormalizeAtBatteryPercent(numbers.back());
        }
        return std::nullopt;
    }

    if (lowered.find("+cind:") != std::string::npos) {
        if (lowered.find('\"') != std::string::npos ||
            lowered.find('(') != std::string::npos ||
            lowered.find('-') != std::string::npos) {
            return std::nullopt;
        }
        return NormalizeCindBatteryPercent(numbers.back());
    }

    if (lowered.find("+ciev") != std::string::npos) {
        return NormalizeCindBatteryPercent(numbers.back());
    }

    if (lowered.find("+xevent") != std::string::npos || lowered.find("+biev") != std::string::npos ||
        lowered.find("battery") != std::string::npos || lowered.find("batt") != std::string::npos) {
        for (auto it = numbers.rbegin(); it != numbers.rend(); ++it) {
            const auto normalized = NormalizeAtBatteryPercent(*it);
            if (normalized.has_value()) {
                return normalized;
            }
        }
    }

    return std::nullopt;
}

std::optional<std::uint8_t> TryReadHfpBatteryFromSocket(SOCKET socket_handle,
                                                        bool debug_enabled,
                                                        HfpDebugLogFn debug_log) {
    if (socket_handle == INVALID_SOCKET) {
        return std::nullopt;
    }

    constexpr std::array<const char*, 13> kProbeCommands = {
        "AT+XAPL=ABCD-1234-0100,7\r",
        "AT+BRSF=20\r",
        "AT+CMER=3,0,0,1\r",
        "AT+CIND=?\r",
        "AT+CIND?\r",
        "AT+IPHONEACCEV?\r",
        "AT+XEVENT?\r",
        "AT+BIEV?\r",
        "AT+XEVENT=BATTERY?\r",
        "AT+XEVENT=BATTERY\r",
        "AT+BATT?\r",
        "AT+BATTERY?\r",
        "AT+QBAT?\r",
    };

    std::string response_text;

    auto drain_input = [&](int rounds) {
        for (int attempt = 0; attempt < rounds; ++attempt) {
            const auto chunk = ReceiveChunk(socket_handle);
            if (!chunk.has_value()) {
                break;
            }
            response_text.append(reinterpret_cast<const char*>(chunk->data()), chunk->size());
        }
    };

    drain_input(2);

    for (const auto* command : kProbeCommands) {
        const std::size_t command_length = std::strlen(command);
        if (send(socket_handle, command, static_cast<int>(command_length), 0) <= 0) {
            continue;
        }
        if (command_length > 0U && command[command_length - 1U] == '\r') {
            std::string command_crlf(command, command_length);
            command_crlf.push_back('\n');
            send(socket_handle, command_crlf.data(), static_cast<int>(command_crlf.size()), 0);
        }

        const std::size_t before_size = response_text.size();
        std::this_thread::sleep_for(std::chrono::milliseconds(90));
        drain_input(3);
        if (debug_enabled && response_text.size() > before_size) {
            const auto delta = response_text.substr(before_size);
            MaybeDebugLog(debug_enabled,
                          debug_log,
                          "HFP fallback command '" +
                              std::string(command, command + static_cast<std::ptrdiff_t>(command_length - 1U)) +
                              "' response chunk: " + delta);
        }

        std::istringstream reader(response_text);
        std::string line;
        while (std::getline(reader, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const auto parsed = ParseAtBatteryPercentFromLine(line);
            if (parsed.has_value()) {
                MaybeDebugLog(debug_enabled, debug_log, "HFP fallback battery parsed from line: '" + line + "'");
                return parsed;
            }
        }

        const auto cind_parsed = TryParseHfpCindBatteryPercent(response_text);
        if (cind_parsed.has_value()) {
            MaybeDebugLog(debug_enabled, debug_log, "HFP fallback battery parsed from +CIND");
            return cind_parsed;
        }
    }

    return std::nullopt;
}

std::vector<BatteryReading> TryReadHfpAgBatteryFromSocket(SOCKET socket_handle,
                                                          bool debug_enabled,
                                                          HfpDebugLogFn debug_log) {
    std::vector<BatteryReading> readings;
    if (socket_handle == INVALID_SOCKET) {
        return readings;
    }

    std::optional<std::uint8_t> main_battery;
    std::string text_buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3200);

    while (std::chrono::steady_clock::now() < deadline) {
        const auto chunk = ReceiveChunk(socket_handle);
        if (!chunk.has_value() || chunk->empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(55));
            continue;
        }

        const std::string chunk_text(reinterpret_cast<const char*>(chunk->data()), chunk->size());
        text_buffer.append(chunk_text);
        MaybeDebugLog(debug_enabled, debug_log, "HFP AG rx='" + chunk_text + "'");

        std::size_t start = 0;
        while (start < text_buffer.size()) {
            const auto line_end = text_buffer.find_first_of("\r\n", start);
            if (line_end == std::string::npos) {
                break;
            }

            std::string line = text_buffer.substr(start, line_end - start);
            start = line_end + 1U;
            while (start < text_buffer.size() &&
                   (text_buffer[start] == '\r' || text_buffer[start] == '\n')) {
                ++start;
            }

            if (line.empty()) {
                continue;
            }

            ReplyToHfpAgCommand(socket_handle, line);

            if (const auto snapshot = ExtractZmiSerialTextSnapshot(line); snapshot.has_value()) {
                readings = BuildXiaomiBatteryReadings(*snapshot);
                if (!readings.empty()) {
                    MaybeDebugLog(debug_enabled, debug_log, "HFP AG battery triplet parsed from line: '" + line + "'");
                    return readings;
                }
            }

            if (const auto parsed = ParseAtBatteryPercentFromLine(line); parsed.has_value()) {
                main_battery = parsed;
            }
        }

        if (start > 0U) {
            text_buffer.erase(0, start);
        }
    }

    if (main_battery.has_value()) {
        readings.push_back(BatteryReading{"main", *main_battery});
    }

    return readings;
}

}  // namespace battery_monitor
