#pragma once

#include <cstdint>
#include <string>

#include "core/INoiseControlProvider.h"
#include "platform/windows/WindowsBluetoothTargetResolver.h"
#include "platform/windows/XiaomiControlConnection.h"

namespace battery_monitor {

struct XiaomiControlActionContext {
    bool debug_enabled = false;
    XiaomiDebugLogFn debug_log = nullptr;
};

bool ProbeXiaomiNoiseControlForTarget(const ResolvedBluetoothTarget& target,
                                      const XiaomiControlActionContext& context);
bool ObserveXiaomiControlSessionForTarget(const ResolvedBluetoothTarget& target,
                                          int duration_seconds,
                                          const XiaomiControlActionContext& context);
bool ObserveZmiSerialSessionForTarget(const ResolvedBluetoothTarget& target,
                                      int duration_seconds,
                                      const XiaomiControlActionContext& context);
bool SetXiaomiNoiseModeForTarget(const ResolvedBluetoothTarget& target,
                                 const std::string& mode,
                                 const XiaomiControlActionContext& context);
bool SendXiaomiControlCandidateForTarget(const ResolvedBluetoothTarget& target,
                                         int candidate_id,
                                         const XiaomiControlActionContext& context);
bool SetXiaomiNoiseSubmodeForTarget(const ResolvedBluetoothTarget& target,
                                    const std::string& family,
                                    int submode,
                                    const XiaomiControlActionContext& context);
bool SetNoiseControlModeForAddress(std::uint64_t address,
                                   NoiseControlMode mode,
                                   const XiaomiControlActionContext& context);

}  // namespace battery_monitor
