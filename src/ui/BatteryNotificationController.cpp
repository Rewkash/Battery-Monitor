#include "ui/BatteryNotificationController.h"

#include <algorithm>

namespace battery_monitor {

void LowBatteryNotifier::SetThresholdPercent(int threshold_percent) {
    threshold_percent_ = std::clamp(threshold_percent, 1, 100);
}

void LowBatteryNotifier::SetRepeatIntervalMinutes(int repeat_minutes) {
    repeat_interval_minutes_ = std::max(0, repeat_minutes);
}

std::vector<LowBatteryAlert> LowBatteryNotifier::Evaluate(
    const std::vector<LowBatteryDeviceSnapshot>& snapshots,
    std::int64_t now_ms) {
    std::vector<LowBatteryAlert> alerts;

    // Reset state for devices that are not present in this cycle: absence is
    // treated like a disconnect so a reconnect starts from a clean slate.
    std::unordered_map<std::string, const LowBatteryDeviceSnapshot*> snapshots_by_key;
    snapshots_by_key.reserve(snapshots.size());
    for (const auto& snapshot : snapshots) {
        if (snapshot.device_key.empty()) {
            continue;
        }
        snapshots_by_key[snapshot.device_key] = &snapshot;
    }

    for (auto it = device_states_.begin(); it != device_states_.end();) {
        const auto snapshot_it = snapshots_by_key.find(it->first);
        const bool connected = snapshot_it != snapshots_by_key.end() && snapshot_it->second->connected;
        if (!connected) {
            it = device_states_.erase(it);
            continue;
        }
        ++it;
    }

    for (const auto& snapshot : snapshots) {
        if (snapshot.device_key.empty() || !snapshot.connected) {
            continue;
        }

        DeviceState& state = device_states_[snapshot.device_key];
        const std::int64_t repeat_interval_ms =
            static_cast<std::int64_t>(repeat_interval_minutes_) * 60LL * 1000LL;

        bool should_alert = false;
        bool any_below = false;
        bool any_above = false;

        for (const auto& reading : snapshot.live_components) {
            const bool below_threshold =
                static_cast<int>(reading.level_percent) <= threshold_percent_;
            if (below_threshold) {
                any_below = true;
            } else {
                any_above = true;
            }

            const auto previous_it = state.previous_levels.find(reading.component);
            const bool crossed_down = previous_it == state.previous_levels.end() ||
                                      static_cast<int>(previous_it->second) > threshold_percent_;
            if (below_threshold && crossed_down) {
                should_alert = true;
            }
            state.previous_levels[reading.component] = reading.level_percent;
        }

        if (any_below && !should_alert && state.has_last_alert) {
            const std::int64_t elapsed_ms = now_ms - state.last_alert_ms;
            if (elapsed_ms >= repeat_interval_ms || elapsed_ms < 0) {
                should_alert = true;
            }
        }

        if (any_above && !any_below) {
            // All components are back above the threshold: allow an immediate
            // alert on the next crossing.
            state.has_last_alert = false;
            state.last_alert_ms = 0;
        }

        if (!should_alert) {
            continue;
        }

        state.last_alert_ms = now_ms;
        state.has_last_alert = true;

        LowBatteryAlert alert;
        alert.device_key = snapshot.device_key;
        alert.device_name = snapshot.device_name;
        alert.critical_level = 100;
        for (const auto& reading : snapshot.live_components) {
            alert.components.push_back(reading);
            if (static_cast<int>(reading.level_percent) <= threshold_percent_) {
                alert.triggered_components.push_back(reading.component);
                if (reading.level_percent < alert.critical_level) {
                    alert.critical_level = reading.level_percent;
                }
            }
        }
        if (alert.triggered_components.empty()) {
            continue;
        }
        if (alert.critical_level == 100) {
            alert.critical_level = static_cast<std::uint8_t>(threshold_percent_);
        }
        alerts.push_back(std::move(alert));
    }

    std::sort(alerts.begin(), alerts.end(), [](const LowBatteryAlert& left, const LowBatteryAlert& right) {
        return left.critical_level < right.critical_level;
    });
    return alerts;
}

void LowBatteryNotifier::ResetDevice(const std::string& device_key) {
    device_states_.erase(device_key);
}

void LowBatteryNotifier::ResetAll() {
    device_states_.clear();
}

}  // namespace battery_monitor
