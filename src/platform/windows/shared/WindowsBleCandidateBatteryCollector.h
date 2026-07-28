#pragma once

#include <string>

#include "platform/windows/shared/WindowsBatteryAggregation.h"
#include "platform/windows/devices/xiaomi/XiaomiBatteryCaches.h"

namespace battery_monitor {

using DeviceBatteryHeuristicFn = bool (*)(const std::string&, const std::string&, const std::string&);

struct WindowsBleCandidateBatteryCollectorContext {
    bool debug_enabled = false;
    XiaomiDebugLogFn debug_log = nullptr;
    std::string target_device_id;
    bool force_live_refresh = false;
    DeviceBatteryHeuristicFn is_likely_tws_device = nullptr;
    DeviceBatteryHeuristicFn is_likely_xiaomi_earbuds = nullptr;
    DeviceBatteryHeuristicFn should_aggressive_xiaomi_classic_retry = nullptr;
};

void CollectBleCandidateBatteryEntries(const WindowsBleCandidateBatteryCollectorContext& context,
                                       DeviceBatteryAccumulator* accumulator,
                                       XiaomiClassicBatteryCache* xiaomi_classic_cache);

}  // namespace battery_monitor

