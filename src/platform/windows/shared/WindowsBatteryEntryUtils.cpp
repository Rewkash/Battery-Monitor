#include "platform/windows/shared/WindowsBatteryEntryUtils.h"

#include "platform/windows/shared/BatteryComponentNaming.h"
#include "platform/windows/shared/BluetoothVisualHintProperties.h"
#include "platform/windows/shared/WindowsDeviceInfoProperties.h"

namespace battery_monitor {

using winrt::Windows::Devices::Enumeration::DeviceInformation;

void PopulateBluetoothVisualHintsFromDeviceInfo(const DeviceInformation& device_info, DeviceBatteryInfo* entry) {
    if (entry == nullptr) {
        return;
    }

    std::uint32_t value32 = 0;
    if (TryGetUInt32Property(device_info, L"System.Devices.Aep.Bluetooth.Le.Appearance", &value32)) {
        entry->bluetooth_le_appearance = static_cast<std::uint16_t>(value32);
    }
    if (TryGetUInt32Property(device_info, L"System.Devices.Aep.Bluetooth.Cod.Major", &value32)) {
        entry->bluetooth_cod_major = value32;
    }
    if (TryGetUInt32Property(device_info, L"System.Devices.Aep.Bluetooth.Cod.Minor", &value32)) {
        entry->bluetooth_cod_minor = value32;
    }

    std::vector<std::string> categories;
    if (TryGetStringArrayProperty(device_info, kDeviceContainerCategoryProperty, &categories)) {
        AppendUniqueStrings(&entry->device_categories, categories);
    }
    if (TryGetStringArrayProperty(device_info, kDeviceContainerPrimaryCategoryProperty, &categories)) {
        AppendUniqueStrings(&entry->device_categories, categories);
    }
    if (TryGetStringArrayProperty(device_info, L"System.Devices.AepContainer.Categories", &categories)) {
        AppendUniqueStrings(&entry->device_categories, categories);
    }
    if (TryGetStringArrayProperty(device_info, L"System.Devices.Aep.Category", &categories)) {
        AppendUniqueStrings(&entry->device_categories, categories);
    }
    if (TryGetStringArrayProperty(device_info, L"System.Devices.Category", &categories)) {
        AppendUniqueStrings(&entry->device_categories, categories);
    }
}

void PopulateBluetoothVisualHintsFromEndpointCandidate(const EndpointCandidate& candidate, DeviceBatteryInfo* entry) {
    if (entry == nullptr) {
        return;
    }
    if (!entry->bluetooth_le_appearance.has_value() && candidate.bluetooth_le_appearance.has_value()) {
        entry->bluetooth_le_appearance = candidate.bluetooth_le_appearance;
    }
    if (!entry->bluetooth_cod_major.has_value() && candidate.bluetooth_cod_major.has_value()) {
        entry->bluetooth_cod_major = candidate.bluetooth_cod_major;
    }
    if (!entry->bluetooth_cod_minor.has_value() && candidate.bluetooth_cod_minor.has_value()) {
        entry->bluetooth_cod_minor = candidate.bluetooth_cod_minor;
    }
    AppendUniqueStrings(&entry->device_categories, candidate.device_categories);
}

void PopulateBluetoothVisualHintsFromDeviceInfo(const DeviceInformation& device_info, EndpointCandidate* candidate) {
    if (candidate == nullptr) {
        return;
    }

    std::uint32_t value32 = 0;
    if (TryGetUInt32Property(device_info, L"System.Devices.Aep.Bluetooth.Le.Appearance", &value32)) {
        candidate->bluetooth_le_appearance = static_cast<std::uint16_t>(value32);
    }
    if (TryGetUInt32Property(device_info, L"System.Devices.Aep.Bluetooth.Cod.Major", &value32)) {
        candidate->bluetooth_cod_major = value32;
    }
    if (TryGetUInt32Property(device_info, L"System.Devices.Aep.Bluetooth.Cod.Minor", &value32)) {
        candidate->bluetooth_cod_minor = value32;
    }

    std::vector<std::string> categories;
    if (TryGetStringArrayProperty(device_info, kDeviceContainerCategoryProperty, &categories)) {
        AppendUniqueStrings(&candidate->device_categories, categories);
    }
    if (TryGetStringArrayProperty(device_info, kDeviceContainerPrimaryCategoryProperty, &categories)) {
        AppendUniqueStrings(&candidate->device_categories, categories);
    }
    if (TryGetStringArrayProperty(device_info, L"System.Devices.AepContainer.Categories", &categories)) {
        AppendUniqueStrings(&candidate->device_categories, categories);
    }
    if (TryGetStringArrayProperty(device_info, L"System.Devices.Aep.Category", &categories)) {
        AppendUniqueStrings(&candidate->device_categories, categories);
    }
    if (TryGetStringArrayProperty(device_info, L"System.Devices.Category", &categories)) {
        AppendUniqueStrings(&candidate->device_categories, categories);
    }
}

void AppendBatteryEntriesFromReadings(std::vector<DeviceBatteryInfo>* entries,
                                      const DeviceInformation& device_info,
                                      const std::string& device_id,
                                      const std::string& device_name,
                                      const std::vector<BatteryReading>& readings,
                                      bool is_connected) {
    if (entries == nullptr) {
        return;
    }

    const std::string resolved_name = device_name.empty() ? "Unknown" : device_name;
    for (const auto& reading : readings) {
        DeviceBatteryInfo entry;
        entry.device_id = device_id;
        entry.device_name = resolved_name;
        entry.battery_component = reading.component.empty() ? "main" : reading.component;
        entry.battery_level_percent = reading.percent;
        PopulateBluetoothVisualHintsFromDeviceInfo(device_info, &entry);
        entry.is_connected = is_connected;
        entries->push_back(std::move(entry));
    }
}

void AppendSingleBatteryEntry(std::vector<DeviceBatteryInfo>* entries,
                              const DeviceInformation& device_info,
                              const std::string& device_id,
                              const std::string& device_name,
                              std::optional<std::uint8_t> battery_percent,
                              bool is_connected) {
    if (entries == nullptr) {
        return;
    }

    DeviceBatteryInfo entry;
    entry.device_id = device_id;
    entry.device_name = device_name.empty() ? "Unknown" : device_name;
    entry.battery_component = NormalizeBatteryComponentHint(entry.device_name);
    if (entry.battery_component.empty()) {
        entry.battery_component = "main";
    }
    entry.battery_level_percent = battery_percent;
    PopulateBluetoothVisualHintsFromDeviceInfo(device_info, &entry);
    entry.is_connected = is_connected;
    entries->push_back(std::move(entry));
}

}  // namespace battery_monitor

