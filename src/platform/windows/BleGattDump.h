#pragma once

#include <string>
#include <vector>

#include <winrt/Windows.Devices.Enumeration.h>

namespace battery_monitor {

bool DumpBleGattForCandidates(
    const std::vector<winrt::Windows::Devices::Enumeration::DeviceInformation>& candidates,
    const std::string& device_hint);

}  // namespace battery_monitor
