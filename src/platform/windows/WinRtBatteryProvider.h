#pragma once

#include "core/IBluetoothBatteryProvider.h"
#include "core/INoiseControlProvider.h"

namespace battery_monitor {

class WinRtBatteryProvider final : public IBluetoothBatteryProvider, public INoiseControlProvider {
   public:
    std::vector<DeviceBatteryInfo> GetDevicesBattery(const BatteryQueryOptions& options) override;
    bool SupportsNoiseControl(const std::string& device_id) override;
    bool SetNoiseControlMode(const std::string& device_id, NoiseControlMode mode) override;
    bool ProbeXiaomiNoiseControl(const std::string& device_hint = std::string());
    bool ObserveXiaomiControlSession(const std::string& device_hint = std::string(), int duration_seconds = 45);
    bool SetXiaomiNoiseMode(const std::string& mode, const std::string& device_hint = std::string());
    bool SendXiaomiControlCandidate(int candidate_id, const std::string& device_hint = std::string());
};

}  // namespace battery_monitor
