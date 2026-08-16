#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.h>

#include "core/BatteryTypes.h"
#include "platform/windows/devices/xiaomi/XiaomiBatteryCodec.h"

namespace battery_monitor {

using BleVendorTripletDebugLogFn = void (*)(const std::string&);

std::vector<BatteryReading> TryReadBleVendorTripletBattery(
    const winrt::Windows::Devices::Bluetooth::BluetoothLEDevice& device,
    bool debug_enabled = false,
    BleVendorTripletDebugLogFn debug_log = nullptr,
    const ProviderOperationContext& operation = {});

std::vector<BatteryReading> TryReadBleFff1Battery(
    const winrt::Windows::Devices::Bluetooth::BluetoothLEDevice& device,
    bool debug_enabled = false,
    BleVendorTripletDebugLogFn debug_log = nullptr,
    const ProviderOperationContext& operation = {});

void CaptureBleNotifyDebugSnapshot(
    const winrt::Windows::Devices::Bluetooth::BluetoothLEDevice& device,
    std::chrono::milliseconds listen_window,
    bool debug_enabled = false,
    BleVendorTripletDebugLogFn debug_log = nullptr,
    const ProviderOperationContext& operation = {});

void CaptureBleGattLayoutDebugSnapshot(
    const winrt::Windows::Devices::Bluetooth::BluetoothLEDevice& device,
    bool debug_enabled = false,
    BleVendorTripletDebugLogFn debug_log = nullptr);

}  // namespace battery_monitor

