#pragma once

#include <cstdint>
#include <string>

#include "core/INoiseControlProvider.h"
#include "platform/windows/shared/WindowsBluetoothTargetResolver.h"
#include "platform/windows/devices/xiaomi/XiaomiControlConnection.h"

namespace battery_monitor {

struct XiaomiControlActionContext {
    bool debug_enabled = false;
    XiaomiDebugLogFn debug_log = nullptr;
};

bool SetXiaomiNoiseModeForTarget(const ResolvedBluetoothTarget& target,
                                 const std::string& mode,
                                 const XiaomiControlActionContext& context);
bool SetXiaomiNoiseSubmodeForTarget(const ResolvedBluetoothTarget& target,
                                    const std::string& family,
                                    int submode,
                                    const XiaomiControlActionContext& context);
bool SetNoiseControlModeForAddress(std::uint64_t address,
                                   NoiseControlMode mode,
                                   const XiaomiControlActionContext& context);

}  // namespace battery_monitor

