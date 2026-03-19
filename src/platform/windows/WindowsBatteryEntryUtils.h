#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Windows.Devices.Enumeration.h>

#include "core/BatteryTypes.h"
#include "platform/windows/XiaomiBatteryCodec.h"
#include "platform/windows/XiaomiHandshake.h"

namespace battery_monitor {

struct EndpointCandidate {
    std::string endpoint_id;
    std::string endpoint_name;
    std::uint64_t bluetooth_address = 0;
    std::optional<std::uint16_t> bluetooth_le_appearance;
    std::optional<std::uint32_t> bluetooth_cod_major;
    std::optional<std::uint32_t> bluetooth_cod_minor;
    std::vector<std::string> device_categories;
    bool from_connected_scan = false;
    bool is_connected = false;
};

void PopulateBluetoothVisualHintsFromDeviceInfo(
    const winrt::Windows::Devices::Enumeration::DeviceInformation& device_info,
    DeviceBatteryInfo* entry);
void PopulateBluetoothVisualHintsFromEndpointCandidate(const EndpointCandidate& candidate, DeviceBatteryInfo* entry);
void PopulateBluetoothVisualHintsFromDeviceInfo(
    const winrt::Windows::Devices::Enumeration::DeviceInformation& device_info,
    EndpointCandidate* candidate);
void AppendBatteryEntriesFromReadings(std::vector<DeviceBatteryInfo>* entries,
                                      const winrt::Windows::Devices::Enumeration::DeviceInformation& device_info,
                                      const std::string& device_id,
                                      const std::string& device_name,
                                      const std::vector<BatteryReading>& readings,
                                      bool is_connected);
void AppendSingleBatteryEntry(std::vector<DeviceBatteryInfo>* entries,
                              const winrt::Windows::Devices::Enumeration::DeviceInformation& device_info,
                              const std::string& device_id,
                              const std::string& device_name,
                              std::optional<std::uint8_t> battery_percent,
                              bool is_connected);
bool TryAppendZmiVendorBatteryEntries(std::vector<DeviceBatteryInfo>* entries,
                                      const winrt::Windows::Devices::Enumeration::DeviceInformation& device_info,
                                      const std::string& device_id,
                                      const std::string& device_name,
                                      std::optional<std::uint64_t> address,
                                      bool is_connected,
                                      bool debug_enabled = false,
                                      XiaomiDebugLogFn debug_log = nullptr,
                                      const char* debug_prefix = nullptr);

}  // namespace battery_monitor
