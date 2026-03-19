#pragma once

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/BatteryTypes.h"
#include "platform/windows/WindowsBluetoothAddressUtils.h"

namespace battery_monitor {

using ResolvedBluetoothTarget = std::pair<std::string, std::uint64_t>;

inline std::string ToLowerAsciiTargetResolver(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

inline bool DeviceBatteryEntryMatchesHint(const DeviceBatteryInfo& entry, const std::string& normalized_hint) {
    if (normalized_hint.empty()) {
        return true;
    }
    const std::string probe = ToLowerAsciiTargetResolver(entry.device_name + " " + entry.device_id);
    return probe.find(normalized_hint) != std::string::npos;
}

template <typename TPredicate>
std::vector<ResolvedBluetoothTarget> CollectResolvedBluetoothTargets(const std::vector<DeviceBatteryInfo>& devices,
                                                                    const std::string& device_hint,
                                                                    TPredicate&& predicate) {
    std::vector<ResolvedBluetoothTarget> candidates;
    std::unordered_set<std::uint64_t> seen_addresses;
    const std::string normalized_hint = ToLowerAsciiTargetResolver(device_hint);

    for (const auto& entry : devices) {
        if (!predicate(entry, normalized_hint)) {
            continue;
        }

        const auto address = ParseBluetoothAddressFromDeviceId(entry.device_id);
        if (!address.has_value() || !seen_addresses.insert(*address).second) {
            continue;
        }
        candidates.emplace_back(entry.device_name, *address);
    }

    return candidates;
}

}  // namespace battery_monitor
