#pragma once

#include <string>

#include "platform/windows/WindowsBatteryAggregation.h"
#include "platform/windows/WindowsBatteryQueryReaders.h"
#include "platform/windows/XiaomiAdvertisementSnapshots.h"
#include "platform/windows/XiaomiBatteryCaches.h"

namespace battery_monitor {

using TwsDeviceHeuristicFn = bool (*)(const std::string&, const std::string&, const std::string&);

struct WindowsTwsCandidateBatteryCollectorContext {
    bool debug_enabled = false;
    XiaomiDebugLogFn debug_log = nullptr;
    bool include_disconnected = false;
    bool force_aep_scan = false;
    TwsDeviceHeuristicFn is_likely_xiaomi_earbuds = nullptr;
    TwsDeviceHeuristicFn is_likely_zmi_purpods = nullptr;
    TwsDeviceHeuristicFn should_aggressive_xiaomi_classic_retry = nullptr;
    OpenBleDeviceByAddressFn open_ble_device_by_address = nullptr;
};

void CollectTwsCandidateBatteryEntries(const WindowsTwsCandidateBatteryCollectorContext& context,
                                       const WindowsBatteryQueryReaderContext& query_reader_context,
                                       DeviceBatteryAccumulator* accumulator,
                                       XiaomiClassicBatteryCache* xiaomi_classic_cache,
                                       XiaomiAdvertisementBatteryCache* xiaomi_advertisement_cache);

}  // namespace battery_monitor
