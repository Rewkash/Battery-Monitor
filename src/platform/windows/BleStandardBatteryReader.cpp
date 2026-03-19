#include "platform/windows/BleStandardBatteryReader.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include "platform/windows/BatteryComponentNaming.h"

namespace battery_monitor {

namespace {

using winrt::Windows::Devices::Bluetooth::BluetoothCacheMode;
using winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristicUuids;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattServiceUuids;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Storage::Streams::DataReader;

template <typename TResult>
std::optional<TResult> WaitForAsyncResult(
    winrt::Windows::Foundation::IAsyncOperation<TResult> operation,
    std::chrono::milliseconds timeout) {
    try {
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (operation.Status() == AsyncStatus::Started && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        if (operation.Status() == AsyncStatus::Started) {
            operation.Cancel();
            return std::nullopt;
        }

        if (operation.Status() != AsyncStatus::Completed) {
            return std::nullopt;
        }

        return operation.GetResults();
    } catch (const winrt::hresult_error&) {
        return std::nullopt;
    }
}

std::string ToUtf8(const winrt::hstring& value) {
    return winrt::to_string(value);
}

}  // namespace

std::vector<BatteryReading> ReadBleBatteryReadings(const BluetoothLEDevice& device, bool prefer_tws_labels) {
    std::vector<BatteryReading> readings;

    const auto service_result =
        WaitForAsyncResult(device.GetGattServicesForUuidAsync(GattServiceUuids::Battery()),
                           std::chrono::milliseconds(1500));
    if (!service_result.has_value() || service_result->Status() != GattCommunicationStatus::Success) {
        return readings;
    }

    const auto services = service_result->Services();
    for (const auto& service : services) {
        const auto characteristic_result = WaitForAsyncResult(
            service.GetCharacteristicsForUuidAsync(GattCharacteristicUuids::BatteryLevel()),
            std::chrono::milliseconds(1200));
        if (!characteristic_result.has_value() ||
            characteristic_result->Status() != GattCommunicationStatus::Success) {
            continue;
        }

        const auto characteristics = characteristic_result->Characteristics();
        for (const auto& characteristic : characteristics) {
            const auto read_result =
                WaitForAsyncResult(characteristic.ReadValueAsync(BluetoothCacheMode::Uncached),
                                   std::chrono::milliseconds(900));
            auto resolved_read_result = read_result;
            if (!resolved_read_result.has_value() ||
                resolved_read_result->Status() != GattCommunicationStatus::Success) {
                resolved_read_result = WaitForAsyncResult(
                    characteristic.ReadValueAsync(BluetoothCacheMode::Cached),
                    std::chrono::milliseconds(900));
            }
            if (!resolved_read_result.has_value() ||
                resolved_read_result->Status() != GattCommunicationStatus::Success) {
                continue;
            }

            const auto reader = DataReader::FromBuffer(resolved_read_result->Value());
            if (reader.UnconsumedBufferLength() < 1U) {
                continue;
            }

            BatteryReading entry;
            entry.component = NormalizeBatteryComponentHint(ToUtf8(characteristic.UserDescription()));
            entry.percent = reader.ReadByte();
            readings.push_back(std::move(entry));
        }
    }

    AssignFallbackBatteryComponents(&readings, prefer_tws_labels);

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
        const int lhs_weight = BatteryComponentSortWeight(lhs.component);
        const int rhs_weight = BatteryComponentSortWeight(rhs.component);
        if (lhs_weight != rhs_weight) {
            return lhs_weight < rhs_weight;
        }
        return lhs.component < rhs.component;
    });

    return deduped;
}

}  // namespace battery_monitor
