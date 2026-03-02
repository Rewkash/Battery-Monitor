#include "platform/windows/WinRtBatteryProvider.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

namespace battery_monitor {

namespace {

using winrt::Windows::Devices::Bluetooth::BluetoothCacheMode;
using winrt::Windows::Devices::Bluetooth::BluetoothConnectionStatus;
using winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristicProperties;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristicUuids;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattServiceUuids;
using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Devices::Enumeration::DeviceInformationKind;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::Storage::Streams::DataReader;

struct BatteryReading {
    std::string component;
    std::uint8_t percent = 0;
};

void EnsureApartmentInitialized() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_changed_state&) {
        // The apartment was already initialized with a different model.
    }
}

std::string ToUtf8(const winrt::hstring& value) {
    return winrt::to_string(value);
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string NormalizeComponentHint(const std::string& hint) {
    if (hint.empty()) {
        return {};
    }

    const std::string lowered = ToLowerAscii(hint);
    if (lowered.find("left") != std::string::npos || lowered == "l" || lowered.find(" l ") != std::string::npos) {
        return "left";
    }
    if (lowered.find("right") != std::string::npos || lowered == "r" || lowered.find(" r ") != std::string::npos) {
        return "right";
    }
    if (lowered.find("case") != std::string::npos || lowered.find("box") != std::string::npos) {
        return "case";
    }
    if (lowered.find("main") != std::string::npos) {
        return "main";
    }

    return {};
}

void AssignFallbackComponents(std::vector<BatteryReading>* readings) {
    if (readings == nullptr || readings->empty()) {
        return;
    }

    if (readings->size() == 1 && readings->front().component.empty()) {
        readings->front().component = "main";
        return;
    }

    std::unordered_set<std::string> used;
    for (const auto& reading : *readings) {
        if (!reading.component.empty()) {
            used.insert(reading.component);
        }
    }

    constexpr std::array<const char*, 3> kPreferredComponents = {"left", "right", "case"};
    std::size_t part_index = 1;

    for (auto& reading : *readings) {
        if (!reading.component.empty()) {
            continue;
        }

        bool assigned = false;
        for (const auto* preferred_component : kPreferredComponents) {
            if (!used.contains(preferred_component)) {
                reading.component = preferred_component;
                used.insert(preferred_component);
                assigned = true;
                break;
            }
        }

        if (!assigned) {
            reading.component = "part" + std::to_string(part_index++);
        }
    }
}

int ComponentSortWeight(const std::string& component) {
    if (component == "left") {
        return 0;
    }
    if (component == "right") {
        return 1;
    }
    if (component == "case") {
        return 2;
    }
    if (component == "main") {
        return 3;
    }
    return 10;
}

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

std::vector<std::uint8_t> ReadBufferBytes(const winrt::Windows::Storage::Streams::IBuffer& buffer) {
    const auto reader = DataReader::FromBuffer(buffer);
    std::vector<std::uint8_t> bytes(reader.UnconsumedBufferLength());
    if (!bytes.empty()) {
        reader.ReadBytes(bytes);
    }
    return bytes;
}

std::optional<std::array<std::uint8_t, 3>> FindLikelyBatteryTriplet(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 3) {
        return std::nullopt;
    }

    for (std::size_t index = 0; index + 2 < bytes.size(); ++index) {
        const std::uint8_t first = bytes[index];
        const std::uint8_t second = bytes[index + 1];
        const std::uint8_t third = bytes[index + 2];

        if (first > 100U || second > 100U || third > 100U) {
            continue;
        }

        if (first == 0U && second == 0U && third == 0U) {
            continue;
        }

        return std::array<std::uint8_t, 3>{first, second, third};
    }

    return std::nullopt;
}

std::vector<BatteryReading> TryReadVendorTripletBattery(const BluetoothLEDevice& device) {
    std::vector<BatteryReading> readings;

    const auto services_result = device.GetGattServicesAsync().get();
    if (services_result.Status() != GattCommunicationStatus::Success) {
        return readings;
    }

    for (const auto& service : services_result.Services()) {
        if (service.Uuid() == GattServiceUuids::Battery()) {
            continue;
        }

        const auto characteristics_result = service.GetCharacteristicsAsync().get();
        if (characteristics_result.Status() != GattCommunicationStatus::Success) {
            continue;
        }

        for (const auto& characteristic : characteristics_result.Characteristics()) {
            const auto properties = characteristic.CharacteristicProperties();
            if ((properties & GattCharacteristicProperties::Read) != GattCharacteristicProperties::Read) {
                continue;
            }

            const auto read_result = characteristic.ReadValueAsync(BluetoothCacheMode::Uncached).get();
            if (read_result.Status() != GattCommunicationStatus::Success) {
                continue;
            }

            const auto bytes = ReadBufferBytes(read_result.Value());
            const auto triplet = FindLikelyBatteryTriplet(bytes);
            if (!triplet.has_value()) {
                continue;
            }

            readings.push_back(BatteryReading{"left", (*triplet)[0]});
            readings.push_back(BatteryReading{"right", (*triplet)[1]});
            readings.push_back(BatteryReading{"case", (*triplet)[2]});
            return readings;
        }
    }

    return readings;
}

std::vector<DeviceBatteryInfo> ReadAssociationEndpointBattery() {
    std::vector<DeviceBatteryInfo> endpoint_entries;

    auto requested_properties = winrt::single_threaded_vector<winrt::hstring>();
    requested_properties.Append(L"System.Devices.Aep.IsConnected");
    requested_properties.Append(L"System.Devices.BatteryLife");
    requested_properties.Append(L"System.Devices.BatteryPlusCharging");
    requested_properties.Append(L"System.Devices.BatteryPlusChargingText");
    requested_properties.Append(L"System.ItemNameDisplay");

    constexpr auto kEndpointSelector =
        LR"((System.Devices.Aep.ProtocolId:="{e0cbf06c-cd8b-4647-bb8a-263b43f0f974}" OR System.Devices.Aep.ProtocolId:="{bb7bb05e-5972-42b5-94fc-76eaa7084d49}"))";

    const auto endpoint_infos =
        DeviceInformation::FindAllAsync(kEndpointSelector, requested_properties, DeviceInformationKind::AssociationEndpoint)
            .get();

    for (const auto& endpoint_info : endpoint_infos) {
        bool is_connected = true;
        bool connected_property = false;
        if (TryGetBooleanProperty(endpoint_info, L"System.Devices.Aep.IsConnected", &connected_property)) {
            is_connected = connected_property;
        }

        if (!is_connected) {
            continue;
        }

        const auto battery_percent = ReadBatteryPercentFromEndpointProperties(endpoint_info);
        if (!battery_percent.has_value()) {
            continue;
        }

        std::string endpoint_name = ToUtf8(endpoint_info.Name());
        if (endpoint_name.empty()) {
            IInspectable display_name_value = nullptr;
            if (TryGetPropertyValue(endpoint_info, L"System.ItemNameDisplay", &display_name_value)) {
                try {
                    endpoint_name = ToUtf8(winrt::unbox_value<winrt::hstring>(display_name_value));
                } catch (const winrt::hresult_error&) {
                }
            }
        }
        if (endpoint_name.empty()) {
            endpoint_name = "Unknown";
        }

        DeviceBatteryInfo entry;
        entry.device_id = ToUtf8(endpoint_info.Id());
        entry.device_name = endpoint_name;
        entry.battery_component = NormalizeComponentHint(endpoint_name);
        if (entry.battery_component.empty()) {
            entry.battery_component = "main";
        }
        entry.battery_level_percent = *battery_percent;
        endpoint_entries.push_back(std::move(entry));
    }

    return endpoint_entries;
}

template <typename TBluetoothDevice>
std::vector<BatteryReading> ReadBatteryReadings(const TBluetoothDevice& device) {
    std::vector<BatteryReading> readings;

    const auto service_result = device.GetGattServicesForUuidAsync(GattServiceUuids::Battery()).get();
    if (service_result.Status() != GattCommunicationStatus::Success) {
        return readings;
    }

    const auto services = service_result.Services();
    for (const auto& service : services) {
        const auto characteristic_result =
            service.GetCharacteristicsForUuidAsync(GattCharacteristicUuids::BatteryLevel()).get();
        if (characteristic_result.Status() != GattCommunicationStatus::Success) {
            continue;
        }

        const auto characteristics = characteristic_result.Characteristics();
        for (const auto& characteristic : characteristics) {
            const auto read_result = characteristic.ReadValueAsync(BluetoothCacheMode::Uncached).get();
            if (read_result.Status() != GattCommunicationStatus::Success) {
                continue;
            }

            const auto reader = DataReader::FromBuffer(read_result.Value());
            if (reader.UnconsumedBufferLength() < 1) {
                continue;
            }

            BatteryReading entry;
            entry.component = NormalizeComponentHint(ToUtf8(characteristic.UserDescription()));
            entry.percent = reader.ReadByte();
            readings.push_back(std::move(entry));
        }
    }

    AssignFallbackComponents(&readings);

    std::unordered_set<std::string> dedupe_keys;
    std::vector<BatteryReading> deduped;
    deduped.reserve(readings.size());

    for (const auto& reading : readings) {
        const std::string key = reading.component + "|" + std::to_string(reading.percent);
        if (!dedupe_keys.insert(key).second) {
            continue;
        }
        deduped.push_back(reading);
    }

    std::sort(deduped.begin(), deduped.end(), [](const BatteryReading& lhs, const BatteryReading& rhs) {
        const int lhs_weight = ComponentSortWeight(lhs.component);
        const int rhs_weight = ComponentSortWeight(rhs.component);
        if (lhs_weight != rhs_weight) {
            return lhs_weight < rhs_weight;
        }
        return lhs.component < rhs.component;
    });

    return deduped;
}

void AddCandidatesFromSelector(const winrt::hstring& selector, std::vector<DeviceInformation>* candidates,
                               std::unordered_set<std::string>* known_ids) {
    if (candidates == nullptr || known_ids == nullptr) {
        return;
    }

    try {
        const auto device_infos = DeviceInformation::FindAllAsync(selector).get();
        for (const auto& info : device_infos) {
            const auto device_id = ToUtf8(info.Id());
            if (!known_ids->insert(device_id).second) {
                continue;
            }
            candidates->push_back(info);
        }
    } catch (const winrt::hresult_error&) {
        // Ignore selector errors and continue with others.
    }
}

std::vector<DeviceInformation> EnumerateCandidateDevices() {
    std::vector<DeviceInformation> candidates;
    std::unordered_set<std::string> known_ids;

    AddCandidatesFromSelector(BluetoothLEDevice::GetDeviceSelectorFromConnectionStatus(BluetoothConnectionStatus::Connected),
                              &candidates, &known_ids);
    AddCandidatesFromSelector(BluetoothLEDevice::GetDeviceSelectorFromPairingState(true), &candidates, &known_ids);
    AddCandidatesFromSelector(BluetoothLEDevice::GetDeviceSelector(), &candidates, &known_ids);

    return candidates;
}

}  // namespace

std::vector<DeviceBatteryInfo> WinRtBatteryProvider::GetConnectedDevicesBattery() {
    EnsureApartmentInitialized();

    std::vector<DeviceBatteryInfo> devices_with_battery;
    std::unordered_set<std::string> known_entries;

    const auto device_infos = EnumerateCandidateDevices();

    for (const auto& device_info : device_infos) {
        try {
            const auto ble_device = BluetoothLEDevice::FromIdAsync(device_info.Id()).get();
            if (!ble_device) {
                continue;
            }

            const auto battery_readings = ReadBatteryReadings(ble_device);
            std::vector<BatteryReading> resolved_readings = battery_readings;
            if (resolved_readings.size() <= 1U) {
                const auto vendor_readings = TryReadVendorTripletBattery(ble_device);
                if (!vendor_readings.empty()) {
                    resolved_readings = vendor_readings;
                }
            }

            if (resolved_readings.empty()) {
                continue;
            }

            std::string device_id = ToUtf8(device_info.Id());
            std::string device_name = ToUtf8(device_info.Name());
            if (device_name.empty()) {
                device_name = ToUtf8(ble_device.Name());
            }
            if (device_name.empty()) {
                device_name = "Unknown";
            }

            for (const auto& battery_reading : resolved_readings) {
                const std::string dedupe_key =
                    device_id + "|" + battery_reading.component + "|" + std::to_string(battery_reading.percent);
                if (!known_entries.insert(dedupe_key).second) {
                    continue;
                }

                DeviceBatteryInfo entry;
                entry.device_id = device_id;
                entry.device_name = device_name;
                entry.battery_component = battery_reading.component;
                entry.battery_level_percent = battery_reading.percent;
                devices_with_battery.push_back(std::move(entry));
            }
        } catch (const winrt::hresult_error&) {
            // Ignore devices that fail to respond and continue with the next one.
        }
    }

    try {
        const auto endpoint_entries = ReadAssociationEndpointBattery();
        for (const auto& endpoint_entry : endpoint_entries) {
            const std::string dedupe_key =
                endpoint_entry.device_id + "|" + endpoint_entry.battery_component + "|" +
                std::to_string(endpoint_entry.battery_level_percent);
            if (!known_entries.insert(dedupe_key).second) {
                continue;
            }
            devices_with_battery.push_back(endpoint_entry);
        }
    } catch (const winrt::hresult_error&) {
        // Endpoint battery properties are optional and may not be available.
    }

    return devices_with_battery;
}

}  // namespace battery_monitor
