#pragma once

#include "core/IBluetoothBatteryProvider.h"
#include "core/INoiseControlProvider.h"

namespace battery_monitor {

class WinRtBatteryProvider final : public IBluetoothBatteryProvider, public INoiseControlProvider {
   public:
    std::vector<DeviceBatteryInfo> GetDevicesBattery(const BatteryQueryOptions& options) override;
    INoiseControlProvider* GetNoiseControlProvider() override { return this; }
    bool SupportsNoiseControl(const std::string& device_id) override;
    bool SetNoiseControlMode(const std::string& device_id, NoiseControlMode mode) override;
    bool SupportsNoiseSubmodes(const std::string& device_id, NoiseControlMode mode) override;
    std::vector<std::pair<std::string, std::string>> GetNoiseSubmodes(const std::string& device_id,
                                                                      NoiseControlMode mode) override;
    bool SetNoiseSubmode(const std::string& device_id,
                         NoiseControlMode mode,
                         const std::string& submode_id) override;
    bool SetXiaomiNoiseMode(const std::string& mode, const std::string& device_hint = std::string());
    bool SetXiaomiNoiseSubmode(const std::string& family, int submode, const std::string& device_hint = std::string());
};

}  // namespace battery_monitor
