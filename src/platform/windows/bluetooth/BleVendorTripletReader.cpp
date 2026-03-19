#include "platform/windows/bluetooth/BleVendorTripletReader.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattServiceUuids;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Storage::Streams::DataReader;

void LogDebug(bool debug_enabled, BleVendorTripletDebugLogFn debug_log, const std::string& message) {
    if (debug_enabled && debug_log != nullptr) {
        debug_log(message);
    }
}

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

std::optional<std::array<std::uint8_t, 3>> FindLikelyBatteryTriplet(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 3U) {
        return std::nullopt;
    }

    for (std::size_t index = 0; index + 2U < bytes.size(); ++index) {
        const std::uint8_t first = bytes[index];
        const std::uint8_t second = bytes[index + 1U];
        const std::uint8_t third = bytes[index + 2U];

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

bool IsPrintableAscii(std::uint8_t value) {
    return value >= 32U && value <= 126U;
}

bool LooksLikeTextPayload(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return false;
    }

    std::size_t printable_count = 0;
    std::size_t alpha_count = 0;
    for (const auto byte : bytes) {
        if (IsPrintableAscii(byte)) {
            ++printable_count;
            if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')) {
                ++alpha_count;
            }
        }
    }

    const bool mostly_printable = (printable_count * 100U) / bytes.size() >= 80U;
    return mostly_printable && alpha_count > 0;
}

bool IsLikelyStandardServiceForMetadata(const std::string& uuid_lower) {
    return uuid_lower == "{00001800-0000-1000-8000-00805f9b34fb}" ||
           uuid_lower == "{00001801-0000-1000-8000-00805f9b34fb}" ||
           uuid_lower == "{0000180a-0000-1000-8000-00805f9b34fb}";
}

bool IsLikelyNameCharacteristic(const std::string& uuid_lower) {
    return uuid_lower == "{00002a00-0000-1000-8000-00805f9b34fb}" ||
           uuid_lower == "{00002a29-0000-1000-8000-00805f9b34fb}" ||
           uuid_lower == "{00002a24-0000-1000-8000-00805f9b34fb}" ||
           uuid_lower == "{00002a25-0000-1000-8000-00805f9b34fb}" ||
           uuid_lower == "{00002a27-0000-1000-8000-00805f9b34fb}" ||
           uuid_lower == "{00002a26-0000-1000-8000-00805f9b34fb}" ||
           uuid_lower == "{00002a28-0000-1000-8000-00805f9b34fb}";
}

}  // namespace

std::vector<BatteryReading> TryReadBleVendorTripletBattery(const BluetoothLEDevice& device,
                                                           bool debug_enabled,
                                                           BleVendorTripletDebugLogFn debug_log) {
    std::vector<BatteryReading> readings;

    const auto services_result =
        WaitForAsyncResult(device.GetGattServicesAsync(), std::chrono::milliseconds(1500));
    if (!services_result.has_value() || services_result->Status() != GattCommunicationStatus::Success) {
        return readings;
    }

    int best_score = -1000;
    std::array<std::uint8_t, 3> best_triplet{0, 0, 0};
    bool found_candidate = false;

    for (const auto& service : services_result->Services()) {
        const std::string service_uuid = ToLowerAscii(ToUtf8(winrt::to_hstring(service.Uuid())));
        if (service.Uuid() == GattServiceUuids::Battery() || IsLikelyStandardServiceForMetadata(service_uuid)) {
            continue;
        }

        const auto characteristics_result =
            WaitForAsyncResult(service.GetCharacteristicsAsync(), std::chrono::milliseconds(1200));
        if (!characteristics_result.has_value() ||
            characteristics_result->Status() != GattCommunicationStatus::Success) {
            continue;
        }

        for (const auto& characteristic : characteristics_result->Characteristics()) {
            const auto properties = characteristic.CharacteristicProperties();
            if ((properties & GattCharacteristicProperties::Read) != GattCharacteristicProperties::Read) {
                continue;
            }

            const std::string characteristic_uuid = ToLowerAscii(ToUtf8(winrt::to_hstring(characteristic.Uuid())));
            if (IsLikelyNameCharacteristic(characteristic_uuid)) {
                continue;
            }

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

            const auto bytes = ReadBufferBytes(resolved_read_result->Value());
            if (bytes.empty() || LooksLikeTextPayload(bytes)) {
                continue;
            }

            LogDebug(debug_enabled,
                     debug_log,
                     "Vendor read service=" + service_uuid +
                         " char=" + characteristic_uuid +
                         " len=" + std::to_string(bytes.size()) +
                         " data=" + BytesToHex(bytes));

            const auto triplet = FindLikelyBatteryTriplet(bytes);
            if (!triplet.has_value()) {
                continue;
            }

            int score = 0;
            if (bytes.size() == 3U) {
                score += 50;
            } else if (bytes.size() <= 8U) {
                score += 20;
            } else if (bytes.size() >= 32U) {
                score -= 10;
            }

            const auto& t = *triplet;
            if (!(t[0] == t[1] && t[1] == t[2])) {
                score += 10;
            }
            if (t[0] == 0U || t[1] == 0U || t[2] == 0U) {
                score -= 5;
            }
            if (t[0] == 100U || t[1] == 100U || t[2] == 100U) {
                score += 5;
            }

            LogDebug(debug_enabled,
                     debug_log,
                     "Vendor triplet candidate: " +
                         std::to_string((*triplet)[0]) + "," +
                         std::to_string((*triplet)[1]) + "," +
                         std::to_string((*triplet)[2]) +
                         " score=" + std::to_string(score));

            if (score > best_score) {
                best_score = score;
                best_triplet = *triplet;
                found_candidate = true;
            }
        }
    }

    if (!found_candidate || best_score < 20) {
        LogDebug(debug_enabled, debug_log, "Vendor triplet was not confident enough.");
        return readings;
    }

    readings.push_back(BatteryReading{"left", best_triplet[0]});
    readings.push_back(BatteryReading{"right", best_triplet[1]});
    readings.push_back(BatteryReading{"case", best_triplet[2]});

    return readings;
}

}  // namespace battery_monitor

