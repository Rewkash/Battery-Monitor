#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "platform/windows/XiaomiBatteryCodec.h"
#include "platform/windows/XiaomiClassicBatterySession.h"
#include "platform/windows/XiaomiHandshake.h"

namespace battery_monitor {

std::optional<std::uint8_t> TryReadGenericClassicHfpBattery(std::uint64_t bluetooth_address,
                                                            bool debug_enabled = false,
                                                            XiaomiDebugLogFn debug_log = nullptr);
std::vector<BatteryReading> TryReadXiaomiClassicBattery(std::uint64_t bluetooth_address,
                                                        bool enable_dynamic_port_scan,
                                                        XiaomiModeCacheUpdateFn mode_cache_update = nullptr,
                                                        int zmi_observe_ms = 0,
                                                        bool debug_enabled = false,
                                                        XiaomiDebugLogFn debug_log = nullptr);

}  // namespace battery_monitor
