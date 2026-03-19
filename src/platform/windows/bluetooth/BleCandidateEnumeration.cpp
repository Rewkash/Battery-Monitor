#include "platform/windows/bluetooth/BleCandidateEnumeration.h"

#include "platform/windows/shared/BluetoothVisualHintProperties.h"

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

namespace battery_monitor {

namespace {

using winrt::Windows::Devices::Bluetooth::BluetoothConnectionStatus;
using winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Foundation::AsyncStatus;

void LogDebug(bool debug_enabled, BleEnumerationDebugLogFn debug_log, const std::string& message) {
    if (debug_enabled && debug_log != nullptr) {
        debug_log(message);
    }
}

std::string ToUtf8(const winrt::hstring& value) {
    return winrt::to_string(value);
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

void AddCandidatesFromSelector(const winrt::hstring& selector,
                               std::vector<DeviceInformation>* candidates,
                               std::unordered_set<std::string>* known_ids,
                               bool debug_enabled,
                               BleEnumerationDebugLogFn debug_log) {
    if (candidates == nullptr || known_ids == nullptr) {
        return;
    }

    try {
        auto requested_properties = winrt::single_threaded_vector<winrt::hstring>();
        requested_properties.Append(L"System.ItemNameDisplay");
        requested_properties.Append(L"System.Devices.Aep.DeviceAddress");
        AppendBluetoothVisualHintPropertyRequests(requested_properties);
        const auto maybe_device_infos =
            WaitForAsyncResult(DeviceInformation::FindAllAsync(selector, requested_properties),
                               std::chrono::milliseconds(1800));
        if (!maybe_device_infos.has_value() || !(*maybe_device_infos)) {
            LogDebug(debug_enabled, debug_log, "Selector scan failed or timed out.");
            return;
        }

        const auto device_infos = *maybe_device_infos;
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

}  // namespace

std::vector<DeviceInformation> EnumerateBleCandidateDevices(bool debug_enabled, BleEnumerationDebugLogFn debug_log) {
    std::vector<DeviceInformation> candidates;
    std::unordered_set<std::string> known_ids;

    try {
        AddCandidatesFromSelector(
            BluetoothLEDevice::GetDeviceSelectorFromConnectionStatus(BluetoothConnectionStatus::Connected),
            &candidates,
            &known_ids,
            debug_enabled,
            debug_log);
    } catch (const winrt::hresult_error& error) {
        LogDebug(debug_enabled, debug_log,
                 "GetDeviceSelectorFromConnectionStatus failed: HRESULT=0x" +
                     std::to_string(static_cast<std::uint32_t>(error.code().value)));
    }

    try {
        AddCandidatesFromSelector(BluetoothLEDevice::GetDeviceSelectorFromPairingState(true),
                                  &candidates,
                                  &known_ids,
                                  debug_enabled,
                                  debug_log);
    } catch (const winrt::hresult_error& error) {
        LogDebug(debug_enabled, debug_log,
                 "GetDeviceSelectorFromPairingState failed: HRESULT=0x" +
                     std::to_string(static_cast<std::uint32_t>(error.code().value)));
    }

    return candidates;
}

}  // namespace battery_monitor

