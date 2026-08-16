#include "platform/windows/shared/BatteryComponentNaming.h"

#include <algorithm>
#include <array>
#include <string>
#include <unordered_set>

namespace battery_monitor {

namespace {

std::string ToLowerAscii(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

}  // namespace

std::string NormalizeBatteryComponentHint(const std::string& hint) {
    if (hint.empty()) {
        return {};
    }

    const std::string lowered = ToLowerAscii(hint);
    if (lowered.find("left") != std::string::npos || lowered == "l" || lowered.find(" l ") != std::string::npos) {
        return "left";
    }
    if (lowered.find("right") != std::string::npos || lowered == "r" || lowered.find(" r ") != std::string::npos) {
        return "right";
    }
    if (lowered.find("case") != std::string::npos || lowered.find("box") != std::string::npos) {
        return "case";
    }
    if (lowered.find("main") != std::string::npos) {
        return "main";
    }

    return {};
}

void AssignFallbackBatteryComponents(std::vector<BatteryReading>* readings,
                                      bool prefer_tws_labels,
                                      XiaomiDebugLogFn debug_log,
                                      const std::string& debug_context) {
    if (readings == nullptr || readings->empty()) {
        return;
    }

    if (readings->size() == 1U && readings->front().component.empty()) {
        readings->front().component = "main";
        return;
    }

    // Deterministic ordering within the snapshot: explicit components first
    // (by sort weight), then unlabeled readings by descending battery level.
    // Ties are readings with identical levels, for which the left/right/case
    // assignment is interchangeable, so the result is order-independent.
    std::stable_sort(readings->begin(), readings->end(),
                     [](const BatteryReading& a, const BatteryReading& b) {
                         const bool a_named = !a.component.empty();
                         const bool b_named = !b.component.empty();
                         if (a_named != b_named) {
                             return a_named;
                         }
                         if (a_named) {
                             return BatteryComponentSortWeight(a.component) <
                                    BatteryComponentSortWeight(b.component);
                         }
                         return a.percent > b.percent;
                     });

    std::unordered_set<std::string> used;
    for (const auto& reading : *readings) {
        if (!reading.component.empty()) {
            used.insert(reading.component);
        }
    }

    const auto log_heuristic = [&](const BatteryReading& reading) {
        if (debug_log != nullptr) {
            debug_log("TWS component heuristic origin='heuristic' component='" + reading.component +
                      "' level=" + std::to_string(reading.percent) +
                      (debug_context.empty() ? std::string() : " context='" + debug_context + "'"));
        }
    };

    constexpr std::array<const char*, 3> kPreferredComponents = {"left", "right", "case"};
    std::size_t part_index = 1U;

    for (auto& reading : *readings) {
        if (!reading.component.empty()) {
            continue;
        }

        if (prefer_tws_labels) {
            bool assigned = false;
            for (const auto* preferred_component : kPreferredComponents) {
                if (!used.contains(preferred_component)) {
                    reading.component = preferred_component;
                    used.insert(preferred_component);
                    assigned = true;
                    break;
                }
            }
            if (assigned) {
                log_heuristic(reading);
                continue;
            }
        }

        reading.component = "part" + std::to_string(part_index++);
        log_heuristic(reading);
    }
}

int BatteryComponentSortWeight(const std::string& component) {
    if (component == "left") {
        return 0;
    }
    if (component == "right") {
        return 1;
    }
    if (component == "case") {
        return 2;
    }
    if (component == "main") {
        return 3;
    }
    return 10;
}

}  // namespace battery_monitor

