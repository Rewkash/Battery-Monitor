#include "platform/windows/BleGattDump.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

namespace battery_monitor {

namespace {

using winrt::Windows::Devices::Bluetooth::BluetoothCacheMode;
using winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristicProperties;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus;
using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Storage::Streams::DataReader;

std::string ToUtf8(const winrt::hstring& value) {
    return winrt::to_string(value);
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string BytesToHex(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return {};
    }

    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(bytes.size() * 3U);
    for (const auto value : bytes) {
        output.push_back(kHex[(value >> 4U) & 0x0FU]);
        output.push_back(kHex[value & 0x0FU]);
        output.push_back(' ');
    }
    return output;
}

std::vector<std::uint8_t> ReadBufferBytes(const winrt::Windows::Storage::Streams::IBuffer& buffer) {
    const auto reader = DataReader::FromBuffer(buffer);
    std::vector<std::uint8_t> bytes(reader.UnconsumedBufferLength());
    if (!bytes.empty()) {
        reader.ReadBytes(bytes);
    }
    return bytes;
}

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

}  // namespace

bool DumpBleGattForCandidates(const std::vector<DeviceInformation>& candidates, const std::string& device_hint) {
    const std::string normalized_hint = ToLowerAscii(device_hint);
    bool dumped_any = false;

    if (candidates.empty()) {
        std::cout << "No BLE candidates matched.\n";
        return false;
    }

    for (const auto& device_info : candidates) {
        const std::string candidate_id = ToUtf8(device_info.Id());
        const std::string candidate_name = ToUtf8(device_info.Name());
        const std::string probe = ToLowerAscii(candidate_name + " " + candidate_id);
        if (!normalized_hint.empty() && probe.find(normalized_hint) == std::string::npos) {
            continue;
        }

        dumped_any = true;
        std::cout << "BLE GATT dump for id='" << candidate_id << "' name='" << candidate_name << "'\n";

        const auto maybe_device =
            WaitForAsyncResult(BluetoothLEDevice::FromIdAsync(device_info.Id()), std::chrono::milliseconds(1800));
        if (!maybe_device.has_value() || !(*maybe_device)) {
            std::cout << "  open by id failed\n";
            continue;
        }
        const auto device = *maybe_device;

        auto services_result =
            WaitForAsyncResult(device.GetGattServicesAsync(BluetoothCacheMode::Uncached),
                               std::chrono::milliseconds(2200));
        if (!services_result.has_value() || services_result->Status() != GattCommunicationStatus::Success) {
            services_result =
                WaitForAsyncResult(device.GetGattServicesAsync(BluetoothCacheMode::Cached),
                                   std::chrono::milliseconds(2200));
        }
        if (!services_result.has_value()) {
            std::cout << "  GATT service enumeration timed out.\n";
            continue;
        }
        if (services_result->Status() != GattCommunicationStatus::Success) {
            std::cout << "  GATT service enumeration failed with status="
                      << static_cast<int>(services_result->Status()) << "\n";
            continue;
        }

        std::cout << "  Services: " << services_result->Services().Size() << "\n";
        for (const auto& service : services_result->Services()) {
            const std::string service_uuid = ToLowerAscii(ToUtf8(winrt::to_hstring(service.Uuid())));
            std::cout << "  service " << service_uuid << "\n";

            auto characteristics_result =
                WaitForAsyncResult(service.GetCharacteristicsAsync(BluetoothCacheMode::Uncached),
                                   std::chrono::milliseconds(1500));
            if (!characteristics_result.has_value() ||
                characteristics_result->Status() != GattCommunicationStatus::Success) {
                characteristics_result =
                    WaitForAsyncResult(service.GetCharacteristicsAsync(BluetoothCacheMode::Cached),
                                       std::chrono::milliseconds(1500));
            }
            if (!characteristics_result.has_value()) {
                std::cout << "    characteristics: timeout\n";
                continue;
            }
            if (characteristics_result->Status() != GattCommunicationStatus::Success) {
                std::cout << "    characteristics: status=" << static_cast<int>(characteristics_result->Status())
                          << "\n";
                continue;
            }

            for (const auto& characteristic : characteristics_result->Characteristics()) {
                const std::string char_uuid = ToLowerAscii(ToUtf8(winrt::to_hstring(characteristic.Uuid())));
                const auto props = static_cast<unsigned int>(characteristic.CharacteristicProperties());
                std::cout << "    char " << char_uuid << " props=0x" << std::hex << props << std::dec;

                const auto user_description = ToUtf8(characteristic.UserDescription());
                if (!user_description.empty()) {
                    std::cout << " desc='" << user_description << "'";
                }
                std::cout << "\n";

                if ((characteristic.CharacteristicProperties() & GattCharacteristicProperties::Read) !=
                    GattCharacteristicProperties::Read) {
                    continue;
                }

                auto read_result =
                    WaitForAsyncResult(characteristic.ReadValueAsync(BluetoothCacheMode::Uncached),
                                       std::chrono::milliseconds(900));
                if (!read_result.has_value() || read_result->Status() != GattCommunicationStatus::Success) {
                    read_result = WaitForAsyncResult(characteristic.ReadValueAsync(BluetoothCacheMode::Cached),
                                                     std::chrono::milliseconds(900));
                }
                if (!read_result.has_value() || read_result->Status() != GattCommunicationStatus::Success) {
                    std::cout << "      read failed\n";
                    continue;
                }

                const auto bytes = ReadBufferBytes(read_result->Value());
                std::cout << "      len=" << bytes.size() << " data=" << BytesToHex(bytes) << "\n";
            }
        }
    }

    if (!dumped_any) {
        std::cout << "No BLE candidates matched.\n";
        return false;
    }

    return true;
}

}  // namespace battery_monitor
