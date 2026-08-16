#pragma once

#include <string>
#include <vector>

#include <winrt/Windows.Devices.Enumeration.h>

#include "core/BatteryTypes.h"

namespace battery_monitor {

using BleEnumerationDebugLogFn = void (*)(const std::string&);

std::vector<winrt::Windows::Devices::Enumeration::DeviceInformation> EnumerateBleCandidateDevices(
    bool debug_enabled = false,
    BleEnumerationDebugLogFn debug_log = nullptr,
    const ProviderOperationContext& operation = {});

}  // namespace battery_monitor
