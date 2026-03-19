#include "platform/windows/WindowsDeviceInfoProperties.h"

#include <algorithm>
#include <array>
#include <exception>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

namespace battery_monitor {

namespace {

using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::Foundation::IPropertyValue;
using winrt::Windows::Foundation::PropertyType;

std::string ToUtf8(const winrt::hstring& value) {
    return winrt::to_string(value);
}

std::string ToLowerAscii(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

std::string GuidToString(const winrt::guid& value) {
    return ToUtf8(winrt::to_hstring(value));
}

std::optional<std::uint8_t> ParsePercentFromText(const std::string& text) {
    int current_number = -1;
    for (const char ch : text) {
        if (ch >= '0' && ch <= '9') {
            if (current_number < 0) {
                current_number = 0;
            }
            current_number = (current_number * 10) + (ch - '0');
            continue;
        }

        if (current_number >= 0) {
            break;
        }
    }

    if (current_number < 0 || current_number > 100) {
        return std::nullopt;
    }

    return static_cast<std::uint8_t>(current_number);
}

}  // namespace

bool TryGetPropertyValue(const DeviceInformation& device_info, const wchar_t* property_name, IInspectable* value) {
    if (value == nullptr) {
        return false;
    }

    const auto properties = device_info.Properties();
    const winrt::hstring key(property_name);
    if (!properties.HasKey(key)) {
        return false;
    }

    *value = properties.Lookup(key);
    return *value != nullptr;
}

bool TryGetBooleanProperty(const DeviceInformation& device_info, const wchar_t* property_name, bool* value) {
    if (value == nullptr) {
        return false;
    }

    IInspectable raw_value = nullptr;
    if (!TryGetPropertyValue(device_info, property_name, &raw_value)) {
        return false;
    }

    try {
        *value = winrt::unbox_value<bool>(raw_value);
        return true;
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

bool TryGetStringProperty(const DeviceInformation& device_info, const wchar_t* property_name, std::string* value) {
    if (value == nullptr) {
        return false;
    }

    IInspectable raw_value = nullptr;
    if (!TryGetPropertyValue(device_info, property_name, &raw_value)) {
        return false;
    }

    try {
        *value = ToUtf8(winrt::unbox_value<winrt::hstring>(raw_value));
        return !value->empty();
    } catch (const winrt::hresult_error&) {
    }

    try {
        *value = GuidToString(winrt::unbox_value<winrt::guid>(raw_value));
        return !value->empty();
    } catch (const winrt::hresult_error&) {
    }

    try {
        *value = std::to_string(winrt::unbox_value<std::uint64_t>(raw_value));
        return !value->empty();
    } catch (const winrt::hresult_error&) {
    }

    try {
        *value = std::to_string(winrt::unbox_value<std::uint32_t>(raw_value));
        return !value->empty();
    } catch (const winrt::hresult_error&) {
    }

    return false;
}

bool TryGetUInt64Property(const DeviceInformation& device_info, const wchar_t* property_name, std::uint64_t* value) {
    if (value == nullptr) {
        return false;
    }

    IInspectable raw_value = nullptr;
    if (!TryGetPropertyValue(device_info, property_name, &raw_value)) {
        return false;
    }

    try {
        *value = winrt::unbox_value<std::uint64_t>(raw_value);
        return true;
    } catch (const winrt::hresult_error&) {
    }

    try {
        *value = static_cast<std::uint64_t>(winrt::unbox_value<std::uint32_t>(raw_value));
        return true;
    } catch (const winrt::hresult_error&) {
    }

    std::string text;
    if (TryGetStringProperty(device_info, property_name, &text)) {
        try {
            *value = std::stoull(text);
            return true;
        } catch (const std::exception&) {
        }
    }

    return false;
}

bool TryGetUInt32Property(const DeviceInformation& device_info, const wchar_t* property_name, std::uint32_t* value) {
    if (value == nullptr) {
        return false;
    }

    IInspectable raw_value = nullptr;
    if (!TryGetPropertyValue(device_info, property_name, &raw_value)) {
        return false;
    }

    try {
        *value = winrt::unbox_value<std::uint32_t>(raw_value);
        return true;
    } catch (const winrt::hresult_error&) {
    }

    try {
        *value = static_cast<std::uint32_t>(winrt::unbox_value<std::uint16_t>(raw_value));
        return true;
    } catch (const winrt::hresult_error&) {
    }

    try {
        *value = static_cast<std::uint32_t>(winrt::unbox_value<std::uint64_t>(raw_value));
        return true;
    } catch (const winrt::hresult_error&) {
    }

    try {
        const auto signed_value = winrt::unbox_value<std::int32_t>(raw_value);
        if (signed_value >= 0) {
            *value = static_cast<std::uint32_t>(signed_value);
            return true;
        }
    } catch (const winrt::hresult_error&) {
    }

    std::string text;
    if (TryGetStringProperty(device_info, property_name, &text)) {
        try {
            *value = static_cast<std::uint32_t>(std::stoul(text));
            return true;
        } catch (const std::exception&) {
        }
    }

    return false;
}

bool TryGetStringArrayProperty(const DeviceInformation& device_info,
                               const wchar_t* property_name,
                               std::vector<std::string>* values) {
    if (values == nullptr) {
        return false;
    }

    IInspectable raw_value = nullptr;
    if (!TryGetPropertyValue(device_info, property_name, &raw_value)) {
        return false;
    }

    if (const auto property_value = raw_value.try_as<IPropertyValue>()) {
        try {
            if (property_value.Type() == PropertyType::StringArray) {
                winrt::com_array<winrt::hstring> array_values;
                property_value.GetStringArray(array_values);
                values->clear();
                values->reserve(array_values.size());
                for (const auto& text : array_values) {
                    const std::string utf8 = ToUtf8(text);
                    if (!utf8.empty()) {
                        values->push_back(utf8);
                    }
                }
                return !values->empty();
            }
        } catch (const winrt::hresult_error&) {
        }
    }

    std::string single_value;
    if (TryGetStringProperty(device_info, property_name, &single_value) && !single_value.empty()) {
        values->assign(1U, single_value);
        return true;
    }

    return false;
}

void AppendUniqueStrings(std::vector<std::string>* target, const std::vector<std::string>& incoming) {
    if (target == nullptr) {
        return;
    }
    for (const auto& value : incoming) {
        if (value.empty()) {
            continue;
        }
        const auto it = std::find(target->begin(), target->end(), value);
        if (it == target->end()) {
            target->push_back(value);
        }
    }
}

bool TryExtractBatteryPercent(const IInspectable& raw_value, std::uint8_t* value) {
    if (value == nullptr || raw_value == nullptr) {
        return false;
    }

    try {
        const auto numeric = winrt::unbox_value<std::uint32_t>(raw_value);
        if (numeric <= 100U) {
            *value = static_cast<std::uint8_t>(numeric);
            return true;
        }
    } catch (const winrt::hresult_error&) {
    }

    try {
        const auto numeric = winrt::unbox_value<std::int32_t>(raw_value);
        if (numeric >= 0 && numeric <= 100) {
            *value = static_cast<std::uint8_t>(numeric);
            return true;
        }
    } catch (const winrt::hresult_error&) {
    }

    try {
        const auto numeric = winrt::unbox_value<std::uint8_t>(raw_value);
        if (numeric <= 100U) {
            *value = static_cast<std::uint8_t>(numeric);
            return true;
        }
    } catch (const winrt::hresult_error&) {
    }

    try {
        const auto text = ToUtf8(winrt::unbox_value<winrt::hstring>(raw_value));
        const auto parsed = ParsePercentFromText(text);
        if (parsed.has_value()) {
            *value = *parsed;
            return true;
        }
    } catch (const winrt::hresult_error&) {
    }

    return false;
}

std::optional<std::uint8_t> ReadBatteryPercentFromEndpointProperties(const DeviceInformation& endpoint_info) {
    constexpr std::array<const wchar_t*, 3> kBatteryProperties = {
        L"System.Devices.BatteryLife",
        L"System.Devices.BatteryPlusCharging",
        L"System.Devices.BatteryPlusChargingText",
    };

    for (const auto* property : kBatteryProperties) {
        IInspectable raw_value = nullptr;
        if (!TryGetPropertyValue(endpoint_info, property, &raw_value)) {
            continue;
        }

        std::uint8_t percent = 0;
        if (TryExtractBatteryPercent(raw_value, &percent)) {
            return percent;
        }
    }

    return std::nullopt;
}

bool IsLikelyBluetoothEndpoint(const DeviceInformation& endpoint_info) {
    const std::string lowered_id = ToLowerAscii(ToUtf8(endpoint_info.Id()));
    if (lowered_id.find("bluetooth") != std::string::npos || lowered_id.find("bthenum") != std::string::npos ||
        lowered_id.find("bthhfenum") != std::string::npos || lowered_id.find("mmdevapi") != std::string::npos) {
        return true;
    }

    std::string protocol_text;
    if (TryGetStringProperty(endpoint_info, L"System.Devices.Aep.ProtocolId", &protocol_text)) {
        const std::string protocol = ToLowerAscii(protocol_text);
        if (protocol.find("e0cbf06c-cd8b-4647-bb8a-263b43f0f974") != std::string::npos ||
            protocol.find("bb7bb05e-5972-42b5-94fc-76eaa7084d49") != std::string::npos) {
            return true;
        }
    }

    return false;
}

bool IsLikelyBluetoothDeviceInfo(const DeviceInformation& device_info) {
    const std::string lowered_id = ToLowerAscii(ToUtf8(device_info.Id()));
    return lowered_id.find("bluetooth") != std::string::npos || lowered_id.find("bthenum") != std::string::npos ||
           lowered_id.find("bthhfenum") != std::string::npos || lowered_id.find("bthledevice") != std::string::npos ||
           lowered_id.find("mmdevapi") != std::string::npos;
}

}  // namespace battery_monitor
