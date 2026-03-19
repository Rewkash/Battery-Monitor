#pragma once

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

namespace battery_monitor {

inline constexpr const wchar_t* kDeviceContainerCategoryProperty =
    L"{78C34FC8-104A-4ACA-9EA4-524D52996E57} 90";
inline constexpr const wchar_t* kDeviceContainerPrimaryCategoryProperty =
    L"{78C34FC8-104A-4ACA-9EA4-524D52996E57} 97";

inline void AppendBluetoothVisualHintPropertyRequests(
    const winrt::Windows::Foundation::Collections::IVector<winrt::hstring>& requested_properties) {
    requested_properties.Append(L"System.Devices.Aep.Bluetooth.Le.Appearance");
    requested_properties.Append(L"System.Devices.Aep.Bluetooth.Cod.Major");
    requested_properties.Append(L"System.Devices.Aep.Bluetooth.Cod.Minor");
    requested_properties.Append(kDeviceContainerCategoryProperty);
    requested_properties.Append(kDeviceContainerPrimaryCategoryProperty);
    requested_properties.Append(L"System.Devices.AepContainer.Categories");
    requested_properties.Append(L"System.Devices.Aep.Category");
    requested_properties.Append(L"System.Devices.Category");
}

}  // namespace battery_monitor
