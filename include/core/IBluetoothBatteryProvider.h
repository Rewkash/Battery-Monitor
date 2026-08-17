#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/BatteryTypes.h"

namespace battery_monitor {

class INoiseControlProvider;

class IBluetoothBatteryProvider {
   public:
    virtual ~IBluetoothBatteryProvider() = default;

    virtual std::vector<DeviceBatteryInfo> GetDevicesBattery(const BatteryQueryOptions& options) = 0;
    virtual INoiseControlProvider* GetNoiseControlProvider() { return nullptr; }
    virtual void NotifyDeviceConnectionChanged(const std::string& device_id, bool connected) {
        (void)device_id;
        (void)connected;
    }
    // Invoked from a background thread when a persistent device session
    // receives fresh data outside of polling (e.g. a Xiaomi protocol push).
    // Implementations must marshal the callback to their own thread.
    virtual void SetDataChangedCallback(std::function<void()> callback) {
        (void)callback;
    }

    std::vector<DeviceBatteryInfo> GetConnectedDevicesBattery() {
        return GetDevicesBattery(BatteryQueryOptions{});
    }
};

}  // namespace battery_monitor
