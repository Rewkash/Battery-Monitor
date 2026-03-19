#pragma once

#include <cstdint>
#include <vector>

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>

#include "platform/windows/XiaomiBatteryCodec.h"
#include "platform/windows/XiaomiHandshake.h"

namespace battery_monitor {

void AppendZmiVendorHintPropertyRequests(
    const winrt::Windows::Foundation::Collections::IVector<winrt::hstring>& requested_properties);
std::vector<BatteryReading> ReadZmiVendorBatteryHintFromPnpAddress(std::uint64_t address,
                                                                   bool debug_enabled = false,
                                                                   XiaomiDebugLogFn debug_log = nullptr);
std::vector<BatteryReading> ReadZmiVendorBatteryHint(
    const winrt::Windows::Devices::Enumeration::DeviceInformation& endpoint_info,
    bool debug_enabled = false,
    XiaomiDebugLogFn debug_log = nullptr);

}  // namespace battery_monitor
