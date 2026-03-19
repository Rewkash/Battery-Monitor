#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>

namespace battery_monitor {

bool TryGetPropertyValue(const winrt::Windows::Devices::Enumeration::DeviceInformation& device_info,
                         const wchar_t* property_name,
                         winrt::Windows::Foundation::IInspectable* value);
bool TryGetBooleanProperty(const winrt::Windows::Devices::Enumeration::DeviceInformation& device_info,
                           const wchar_t* property_name,
                           bool* value);
bool TryGetStringProperty(const winrt::Windows::Devices::Enumeration::DeviceInformation& device_info,
                          const wchar_t* property_name,
                          std::string* value);
bool TryGetUInt64Property(const winrt::Windows::Devices::Enumeration::DeviceInformation& device_info,
                          const wchar_t* property_name,
                          std::uint64_t* value);
bool TryGetUInt32Property(const winrt::Windows::Devices::Enumeration::DeviceInformation& device_info,
                          const wchar_t* property_name,
                          std::uint32_t* value);
bool TryGetStringArrayProperty(const winrt::Windows::Devices::Enumeration::DeviceInformation& device_info,
                               const wchar_t* property_name,
                               std::vector<std::string>* values);
void AppendUniqueStrings(std::vector<std::string>* target, const std::vector<std::string>& incoming);
bool TryExtractBatteryPercent(const winrt::Windows::Foundation::IInspectable& raw_value, std::uint8_t* value);
std::optional<std::uint8_t> ReadBatteryPercentFromEndpointProperties(
    const winrt::Windows::Devices::Enumeration::DeviceInformation& endpoint_info);
bool IsLikelyBluetoothEndpoint(const winrt::Windows::Devices::Enumeration::DeviceInformation& endpoint_info);
bool IsLikelyBluetoothDeviceInfo(const winrt::Windows::Devices::Enumeration::DeviceInformation& device_info);

}  // namespace battery_monitor
