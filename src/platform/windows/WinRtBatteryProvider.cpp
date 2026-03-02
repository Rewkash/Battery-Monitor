#include "platform/windows/WinRtBatteryProvider.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

namespace battery_monitor {

namespace {

using winrt::Windows::Devices::Bluetooth::BluetoothCacheMode;
using winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristicUuids;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattServiceUuids;
using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Storage::Streams::DataReader;

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

std::optional<std::uint8_t> ReadBatteryPercent(const BluetoothLEDevice& device) {
    const auto service_result = device.GetGattServicesForUuidAsync(GattServiceUuids::Battery()).get();
    if (service_result.Status() != GattCommunicationStatus::Success) {
        return std::nullopt;
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

            return reader.ReadByte();
        }
    }

    return std::nullopt;
}

}  // namespace

std::vector<DeviceBatteryInfo> WinRtBatteryProvider::GetConnectedDevicesBattery() {
    EnsureApartmentInitialized();

    std::vector<DeviceBatteryInfo> devices_with_battery;

    const auto selector = BluetoothLEDevice::GetDeviceSelectorFromPairingState(true);
    const auto device_infos = DeviceInformation::FindAllAsync(selector).get();

    for (const auto& device_info : device_infos) {
        try {
            const auto ble_device = BluetoothLEDevice::FromIdAsync(device_info.Id()).get();
            if (!ble_device) {
                continue;
            }

            const auto battery_percent = ReadBatteryPercent(ble_device);
            if (!battery_percent.has_value()) {
                continue;
            }

            DeviceBatteryInfo entry;
            entry.device_id = ToUtf8(device_info.Id());
            entry.device_name = ToUtf8(device_info.Name());
            if (entry.device_name.empty()) {
                entry.device_name = "Unknown";
            }
            entry.battery_level_percent = *battery_percent;
            devices_with_battery.push_back(std::move(entry));
        } catch (const winrt::hresult_error&) {
            // Ignore devices that fail to respond and continue with the next one.
        }
    }

    return devices_with_battery;
}

}  // namespace battery_monitor
