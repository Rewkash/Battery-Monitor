#include "platform/windows/devices/xiaomi/XiaomiBatteryCodec.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace battery_monitor {

std::optional<std::uint8_t> ParseXiaomiBatteryRaw(std::uint8_t raw_value) {
    if (raw_value == 0xFFU) {
        return std::nullopt;
    }
    const auto level = static_cast<std::uint8_t>(raw_value & 0x7FU);
    if (level > 100U) {
        return std::nullopt;
    }
    return level;
}

int XiaomiBatteryPresenceCount(const XiaomiBatterySnapshot& snapshot) {
    int count = 0;
    if (snapshot.left.has_value()) {
        ++count;
    }
    if (snapshot.right.has_value()) {
        ++count;
    }
    if (snapshot.case_level.has_value()) {
        ++count;
    }
    return count;
}

// Structured TLV walk for the newer "Vela" firmware replies (e.g. Redmi
// Buds 8 Pro): entries are [len][tag][value...] where len counts the tag
// byte. Battery status arrives as tag 0x07 with [left, right, case] levels
// (0xFF = component absent, e.g. earbud in the closed case) and tag 0x02
// with the currently active level. The legacy byte-scan below cannot read
// these replies: it misaligns on nested TLV headers and drops the right
// earbud and the case.
std::optional<XiaomiBatterySnapshot> ParseVelaBatteryTlv(const std::vector<std::uint8_t>& payload,
                                                         bool debug_enabled,
                                                         XiaomiBatteryDebugLogFn debug_log) {
    std::optional<XiaomiBatterySnapshot> result;
    std::size_t index = 0;
    while (index + 1U < payload.size()) {
        // Entry [index .. index+len] inclusive: the length byte plus len
        // following bytes (tag + value).
        const std::size_t len = payload[index];
        if (len < 1U || index + len >= payload.size()) {
            break;
        }
        const std::uint8_t tag = payload[index + 1U];
        const std::size_t value_size = len - 1U;
        const std::size_t value_start = index + 2U;

        if (tag == 0x07U && value_size >= 3U) {
            XiaomiBatterySnapshot snapshot;
            snapshot.left = ParseXiaomiBatteryRaw(payload[value_start]);
            snapshot.right = ParseXiaomiBatteryRaw(payload[value_start + 1U]);
            snapshot.case_level = ParseXiaomiBatteryRaw(payload[value_start + 2U]);
            if (XiaomiBatteryPresenceCount(snapshot) > 0) {
                if (debug_enabled && debug_log != nullptr) {
                    const auto level_text = [](const std::optional<std::uint8_t>& level) {
                        return level.has_value() ? std::to_string(*level) : std::string("na");
                    };
                    debug_log("Xiaomi Vela battery TLV tag=07 level=(" + level_text(snapshot.left) + "," +
                              level_text(snapshot.right) + "," + level_text(snapshot.case_level) + ")");
                }
                if (!result.has_value()) {
                    result = snapshot;
                } else {
                    if (snapshot.left.has_value()) result->left = snapshot.left;
                    if (snapshot.right.has_value()) result->right = snapshot.right;
                    if (snapshot.case_level.has_value()) result->case_level = snapshot.case_level;
                }
            }
        }

        index += len + 1U;
    }
    return result;
}


std::optional<XiaomiBatterySnapshot> ExtractBatterySnapshotFromXiaomiPayload(
    const std::vector<std::uint8_t>& payload,
    std::optional<std::uint8_t> preferred_tag,
    bool debug_enabled,
    XiaomiBatteryDebugLogFn debug_log) {
    struct Candidate {
        XiaomiBatterySnapshot snapshot;
        std::uint8_t tag = 0;
        int score = 0;
    };

    std::optional<Candidate> best_candidate;

    // Structured Vela TLVs win over the legacy heuristic scan.
    if (const auto vela = ParseVelaBatteryTlv(payload, debug_enabled, debug_log); vela.has_value()) {
        return *vela;
    }

    std::size_t index = 0;
    while (index + 1U < payload.size()) {
        const std::size_t len = payload[index];
        if (len == 0U || index + len >= payload.size()) {
            ++index;
            continue;
        }

        const std::uint8_t tag = payload[index + 1U];
        if (len < 4U || len > 8U || tag > 0x7FU) {
            index += len + 1U;
            continue;
        }

        const std::uint8_t left_raw = payload[index + 2U];
        const std::uint8_t right_raw = payload[index + 3U];
        const std::uint8_t case_raw = payload[index + 4U];

        XiaomiBatterySnapshot snapshot;
        snapshot.left = ParseXiaomiBatteryRaw(left_raw);
        snapshot.right = ParseXiaomiBatteryRaw(right_raw);
        snapshot.case_level = ParseXiaomiBatteryRaw(case_raw);

        if (!snapshot.left.has_value() && !snapshot.right.has_value() && !snapshot.case_level.has_value()) {
            index += len + 1U;
            continue;
        }

        int score = 0;
        if (preferred_tag.has_value() && tag == *preferred_tag) {
            score += 50;
        }
        if (!preferred_tag.has_value() &&
            (tag == 0x00U || tag == 0x07U || tag == 0x09U || tag == 0x0AU)) {
            score += 12;
        }

        score += 15;

        const int presence = XiaomiBatteryPresenceCount(snapshot);
        score += presence * 10;
        if (snapshot.left.has_value() && snapshot.right.has_value() && *snapshot.left != *snapshot.right) {
            score += 4;
        }
        if ((snapshot.left.has_value() && *snapshot.left == 100U) ||
            (snapshot.right.has_value() && *snapshot.right == 100U) ||
            (snapshot.case_level.has_value() && *snapshot.case_level == 100U)) {
            score += 2;
        }

        if (debug_enabled && debug_log != nullptr) {
            const std::string left_text = snapshot.left.has_value() ? std::to_string(*snapshot.left) : "na";
            const std::string right_text = snapshot.right.has_value() ? std::to_string(*snapshot.right) : "na";
            const std::string case_text = snapshot.case_level.has_value() ? std::to_string(*snapshot.case_level) : "na";
            debug_log("Xiaomi payload battery candidate tag=" + std::to_string(tag) +
                      " raw=(" + std::to_string(left_raw) + "," + std::to_string(right_raw) + "," +
                      std::to_string(case_raw) + ")" +
                      " level=(" + left_text + "," + right_text + "," + case_text + ")" +
                      " score=" + std::to_string(score));
        }

        if (!best_candidate.has_value() || score > best_candidate->score) {
            best_candidate = Candidate{snapshot, tag, score};
        }

        if (preferred_tag.has_value() && tag == *preferred_tag && score >= 70) {
            return snapshot;
        }

        index += len + 1U;
    }

    if (best_candidate.has_value()) {
        return best_candidate->snapshot;
    }

    return std::nullopt;
}

std::optional<XiaomiBatterySnapshot> ExtractPreferredXiaomiBatterySnapshot(
    const std::vector<std::uint8_t>& payload,
    bool debug_enabled,
    XiaomiBatteryDebugLogFn debug_log) {
    if (const auto preferred_device_info =
            ExtractBatterySnapshotFromXiaomiPayload(
                payload, std::optional<std::uint8_t>{static_cast<std::uint8_t>(0x07U)}, debug_enabled, debug_log);
        preferred_device_info.has_value()) {
        return preferred_device_info;
    }

    if (const auto preferred_status =
            ExtractBatterySnapshotFromXiaomiPayload(
                payload, std::optional<std::uint8_t>{static_cast<std::uint8_t>(0x00U)}, debug_enabled, debug_log);
        preferred_status.has_value()) {
        return preferred_status;
    }

    return ExtractBatterySnapshotFromXiaomiPayload(payload, std::nullopt, debug_enabled, debug_log);
}

XiaomiBatterySnapshot MergeXiaomiSnapshots(const XiaomiBatterySnapshot& preferred,
                                          const XiaomiBatterySnapshot& fallback) {
    XiaomiBatterySnapshot merged = preferred;
    if (!merged.left.has_value()) {
        merged.left = fallback.left;
    }
    if (!merged.right.has_value()) {
        merged.right = fallback.right;
    }
    if (!merged.case_level.has_value()) {
        merged.case_level = fallback.case_level;
    }
    return merged;
}

std::vector<BatteryReading> BuildXiaomiBatteryReadings(const XiaomiBatterySnapshot& snapshot) {
    std::vector<BatteryReading> readings;
    if (snapshot.left.has_value()) {
        readings.push_back(BatteryReading{"left", *snapshot.left});
    }
    if (snapshot.right.has_value()) {
        readings.push_back(BatteryReading{"right", *snapshot.right});
    }
    if (snapshot.case_level.has_value()) {
        readings.push_back(BatteryReading{"case", *snapshot.case_level});
    }
    return readings;
}

bool HasAnyBattery(const XiaomiBatterySnapshot& snapshot) {
    return snapshot.left.has_value() || snapshot.right.has_value() || snapshot.case_level.has_value();
}

}  // namespace battery_monitor

