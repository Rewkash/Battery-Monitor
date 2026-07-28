#pragma once

#include <cstdint>
#include <vector>

#include "platform/windows/devices/xiaomi/XiaomiBatteryCodec.h"
#include "platform/windows/devices/xiaomi/XiaomiClassicBatterySession.h"
#include "platform/windows/devices/xiaomi/XiaomiHandshake.h"

namespace battery_monitor {

enum class ClassicBatteryService {
    kXiaomiDeviceControl,
    kBluetoothSerialPort,
    kZmiPurPodsSerial,
};

std::vector<BatteryReading> TryReadXiaomiClassicBattery(std::uint64_t bluetooth_address,
                                                        ClassicBatteryService service,
                                                        bool* connected,
                                                        XiaomiModeCacheUpdateFn mode_cache_update = nullptr,
                                                        bool debug_enabled = false,
                                                        XiaomiDebugLogFn debug_log = nullptr);

}  // namespace battery_monitor

