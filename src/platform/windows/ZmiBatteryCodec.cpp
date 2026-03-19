#include "platform/windows/ZmiBatteryCodec.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace battery_monitor {

namespace {

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

}  // namespace

std::optional<std::uint8_t> NormalizeZmiVendorBatteryScalar(int raw_value) {
    if (raw_value <= 0 || raw_value == 255) {
        return std::nullopt;
    }
    if (raw_value <= 9) {
        return static_cast<std::uint8_t>(std::min(100, raw_value * 10));
    }
    if (raw_value <= 100) {
        return static_cast<std::uint8_t>(raw_value);
    }
    if (raw_value <= 0x7F) {
        const int masked = raw_value & 0x7F;
        if (masked > 0 && masked <= 100) {
            return static_cast<std::uint8_t>(masked);
        }
    }
    return std::nullopt;
}

std::optional<XiaomiBatterySnapshot> ExtractZmiSerialPatternSnapshot(
    const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 7U) {
        return std::nullopt;
    }

    constexpr std::array<std::uint8_t, 4> kPattern = {0x02, 0x02, 0x04, 0x07};
    for (std::size_t index = 0; index + 6U < bytes.size(); ++index) {
        if (!(bytes[index] == kPattern[0] &&
              bytes[index + 1U] == kPattern[1] &&
              bytes[index + 2U] == kPattern[2] &&
              bytes[index + 3U] == kPattern[3])) {
            continue;
        }

        XiaomiBatterySnapshot snapshot;
        snapshot.left = ParseXiaomiBatteryRaw(bytes[index + 4U]);
        snapshot.right = ParseXiaomiBatteryRaw(bytes[index + 5U]);
        snapshot.case_level = ParseXiaomiBatteryRaw(bytes[index + 6U]);
        if (XiaomiBatteryPresenceCount(snapshot) >= 2) {
            return snapshot;
        }
    }

    return std::nullopt;
}

std::optional<XiaomiBatterySnapshot> ExtractZmiSerialTextSnapshot(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }

    const std::string lowered = ToLowerAscii(text);
    const bool has_component_hints =
        lowered.find("left") != std::string::npos ||
        lowered.find("right") != std::string::npos ||
        lowered.find("case") != std::string::npos ||
        lowered.find(" l:") != std::string::npos ||
        lowered.find(" r:") != std::string::npos ||
        lowered.find(" c:") != std::string::npos ||
        lowered.find("l=") != std::string::npos ||
        lowered.find("r=") != std::string::npos ||
        lowered.find("c=") != std::string::npos;
    const bool has_triplet_probe_hint =
        lowered.find("+xevent") != std::string::npos ||
        lowered.find("xevent:") != std::string::npos ||
        lowered.find("+batt:") != std::string::npos ||
        lowered.find("battery:") != std::string::npos ||
        lowered.find("battery,") != std::string::npos;
    if (!has_component_hints && !has_triplet_probe_hint) {
        return std::nullopt;
    }

    std::vector<int> numbers;
    int current = -1;
    for (const char ch : lowered) {
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

    if (numbers.size() < 3U) {
        return std::nullopt;
    }

    XiaomiBatterySnapshot best_snapshot;
    int best_presence = -1;
    for (std::size_t index = 0; index + 2U < numbers.size(); ++index) {
        XiaomiBatterySnapshot snapshot;
        snapshot.left = NormalizeZmiVendorBatteryScalar(numbers[index]);
        snapshot.right = NormalizeZmiVendorBatteryScalar(numbers[index + 1U]);
        snapshot.case_level = NormalizeZmiVendorBatteryScalar(numbers[index + 2U]);

        const int presence = XiaomiBatteryPresenceCount(snapshot);
        if (presence > best_presence) {
            best_presence = presence;
            best_snapshot = snapshot;
        }
    }

    if (best_presence >= 2) {
        return best_snapshot;
    }

    return std::nullopt;
}

}  // namespace battery_monitor
