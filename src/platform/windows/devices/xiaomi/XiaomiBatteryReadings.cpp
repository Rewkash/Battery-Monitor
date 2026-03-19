#include "platform/windows/devices/xiaomi/XiaomiBatteryReadings.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
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

std::size_t XiaomiResolvedTwsComponentCount(const std::vector<BatteryReading>& readings) {
    bool has_left = false;
    bool has_right = false;
    bool has_case = false;

    for (const auto& reading : readings) {
        const std::string component = ToLowerAscii(reading.component);
        if (component == "left") {
            has_left = true;
        } else if (component == "right") {
            has_right = true;
        } else if (component == "case") {
            has_case = true;
        }
    }

    std::size_t count = 0;
    if (has_left) {
        ++count;
    }
    if (has_right) {
        ++count;
    }
    if (has_case) {
        ++count;
    }
    return count;
}

bool HasUsefulXiaomiTwsReadings(const std::vector<BatteryReading>& readings,
                                std::size_t min_components) {
    return XiaomiResolvedTwsComponentCount(readings) >= min_components;
}

int XiaomiReadingsRichnessScore(const std::vector<BatteryReading>& readings) {
    const auto tws_components = static_cast<int>(XiaomiResolvedTwsComponentCount(readings));
    int score = tws_components * 100;
    for (const auto& reading : readings) {
        const std::string component = ToLowerAscii(reading.component);
        if (component == "main") {
            score += 10;
        }
    }
    score += static_cast<int>(std::min<std::size_t>(readings.size(), 9U));
    return score;
}

}  // namespace battery_monitor

