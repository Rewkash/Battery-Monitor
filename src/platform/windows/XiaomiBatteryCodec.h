#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace battery_monitor {

struct BatteryReading {
    std::string component;
    std::uint8_t percent = 0;
};

struct XiaomiBatterySnapshot {
    std::optional<std::uint8_t> left;
    std::optional<std::uint8_t> right;
    std::optional<std::uint8_t> case_level;
};

using XiaomiBatteryDebugLogFn = void (*)(const std::string&);

std::optional<std::uint8_t> ParseXiaomiBatteryRaw(std::uint8_t raw_value);
int XiaomiBatteryPresenceCount(const XiaomiBatterySnapshot& snapshot);
std::optional<XiaomiBatterySnapshot> ExtractBatterySnapshotFromXiaomiPayload(
    const std::vector<std::uint8_t>& payload,
    std::optional<std::uint8_t> preferred_tag,
    bool debug_enabled,
    XiaomiBatteryDebugLogFn debug_log);
std::optional<XiaomiBatterySnapshot> ExtractPreferredXiaomiBatterySnapshot(
    const std::vector<std::uint8_t>& payload,
    bool debug_enabled,
    XiaomiBatteryDebugLogFn debug_log);
XiaomiBatterySnapshot MergeXiaomiSnapshots(const XiaomiBatterySnapshot& preferred,
                                          const XiaomiBatterySnapshot& fallback);
std::vector<BatteryReading> BuildXiaomiBatteryReadings(const XiaomiBatterySnapshot& snapshot);
bool HasAnyBattery(const XiaomiBatterySnapshot& snapshot);

}  // namespace battery_monitor
