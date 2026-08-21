#pragma once

#include <string>
#include <vector>

namespace battery_monitor {

struct BatteryWindowPersistedState {
    int refresh_interval_ms = 15000;
    int low_battery_threshold_percent = 10;
    int low_battery_repeat_minutes = 10;
    std::vector<std::string> connected_device_order;
    std::vector<std::string> disconnected_device_order;
};

inline constexpr int kBatteryWindowDefaultRefreshIntervalMs = 15000;
inline constexpr int kBatteryWindowMinRefreshIntervalSeconds = 1;
inline constexpr int kBatteryWindowMaxRefreshIntervalSeconds = 600;
inline constexpr int kBatteryWindowDefaultLowBatteryThresholdPercent = 10;
inline constexpr int kBatteryWindowMinLowBatteryThresholdPercent = 1;
inline constexpr int kBatteryWindowMaxLowBatteryThresholdPercent = 100;
inline constexpr int kBatteryWindowDefaultLowBatteryRepeatMinutes = 10;
inline constexpr int kBatteryWindowMinLowBatteryRepeatMinutes = 1;
inline constexpr int kBatteryWindowMaxLowBatteryRepeatMinutes = 180;

int ClampBatteryWindowRefreshIntervalMs(int interval_ms);
int ClampBatteryWindowLowBatteryThresholdPercent(int percent);
int ClampBatteryWindowLowBatteryRepeatMinutes(int minutes);
BatteryWindowPersistedState LoadBatteryWindowPersistedState();
void SaveBatteryWindowRefreshIntervalMs(int interval_ms);
void SaveBatteryWindowLowBatteryThresholdPercent(int percent);
void SaveBatteryWindowLowBatteryRepeatMinutes(int minutes);
void SaveBatteryWindowDeviceOrder(const std::vector<std::string>& connected_order,
                                  const std::vector<std::string>& disconnected_order);

// Windows autostart (per-user Run registry key). The value points at the
// current executable; the app starts hidden in the tray by default.
bool IsApplicationAutostartEnabled();
bool SetApplicationAutostartEnabled(bool enabled);

}  // namespace battery_monitor
