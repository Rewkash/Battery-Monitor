#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace battery_monitor {

// Pure, unit-testable decision core for low-battery notifications.
// The class tracks per-physical-device state in memory only; suppression is
// never persisted across app restarts. Suppression is keyed by the physical
// device (device_key), not by individual components (left/right/case), so
// simultaneous component updates produce at most one notification per device.
struct LowBatteryComponentReading {
    std::string component;
    std::uint8_t level_percent = 0;
};

struct LowBatteryDeviceSnapshot {
    std::string device_key;  // stable key of the physical device
    std::string device_name;
    bool connected = false;
    // Live (non-cached) component readings; empty when disconnected or when
    // only cached values are available for this cycle.
    std::vector<LowBatteryComponentReading> live_components;
};

struct LowBatteryAlert {
    std::string device_key;
    std::string device_name;
    // All components seen this cycle (live readings), used for context lines.
    std::vector<LowBatteryComponentReading> components;
    // Components whose current level triggered this alert.
    std::vector<std::string> triggered_components;
    std::uint8_t critical_level = 100;
};

class LowBatteryNotifier {
   public:
    void SetThresholdPercent(int threshold_percent);
    void SetRepeatIntervalMinutes(int repeat_minutes);

    // Evaluates the given snapshots and returns the alerts that should be
    // presented now. Devices that are reported disconnected (or absent from
    // the snapshot list) have their suppression state reset, so a later
    // reconnect while still below the threshold notifies again.
    std::vector<LowBatteryAlert> Evaluate(const std::vector<LowBatteryDeviceSnapshot>& snapshots,
                                          std::int64_t now_ms);

    void ResetDevice(const std::string& device_key);
    void ResetAll();

    int threshold_percent() const { return threshold_percent_; }
    int repeat_interval_minutes() const { return repeat_interval_minutes_; }

   private:
    struct DeviceState {
        std::unordered_map<std::string, std::uint8_t> previous_levels;
        std::int64_t last_alert_ms = 0;
        bool has_last_alert = false;
    };

    std::unordered_map<std::string, DeviceState> device_states_;
    int threshold_percent_ = 20;
    int repeat_interval_minutes_ = 15;
};

}  // namespace battery_monitor
