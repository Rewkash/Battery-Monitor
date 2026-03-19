#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.h>

namespace battery_monitor {

std::optional<std::uint64_t> ParseBluetoothAddress(const std::string& value);
std::vector<std::uint64_t> ParseBluetoothAddressesFromDeviceId(const std::string& device_id);
std::optional<std::uint64_t> ParseBluetoothAddressFromDeviceId(const std::string& device_id);
std::optional<std::uint16_t> ReadBluetoothProductIdFromRegistry(std::uint64_t address);
std::optional<std::uint64_t> TryGetBluetoothAddress(
    const winrt::Windows::Devices::Bluetooth::BluetoothLEDevice& ble_device);

}  // namespace battery_monitor
