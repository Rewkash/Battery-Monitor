#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/BatteryTypes.h"
#include "platform/windows/shared/WindowsBatteryEntryUtils.h"
#include "platform/windows/devices/xiaomi/XiaomiHandshake.h"

namespace battery_monitor {

using DeviceQueryHeuristicFn = bool (*)(const std::string&, const std::string&, const std::string&);
using NameQueryHeuristicFn = bool (*)(const std::string&);
using AddressBatteryReaderFn = std::optional<std::uint8_t> (*)(std::uint64_t);
using ControllerBatteryReaderFn = std::optional<std::uint8_t> (*)(const std::string&, const std::string&);

struct WindowsBatteryQueryReaderContext {
    bool debug_enabled = false;
    XiaomiDebugLogFn debug_log = nullptr;
    DeviceQueryHeuristicFn is_likely_tws_device = nullptr;
    DeviceQueryHeuristicFn is_likely_phone_device = nullptr;
    DeviceQueryHeuristicFn is_likely_game_controller_device = nullptr;
    NameQueryHeuristicFn looks_like_tws_device_by_name = nullptr;
    AddressBatteryReaderFn read_phone_hfp_pnp_hint = nullptr;
    AddressBatteryReaderFn read_zmi_vendor_pnp_hint = nullptr;
    DeviceQueryHeuristicFn is_likely_zmi_device = nullptr;
    ControllerBatteryReaderFn read_controller_battery = nullptr;
};

std::vector<DeviceBatteryInfo> ReadConnectedBluetoothDeviceBatteryFast(
    const WindowsBatteryQueryReaderContext& context,
    std::vector<EndpointCandidate>* tws_candidates,
    const std::string& target_device_id = std::string(),
    const ProviderOperationContext& operation = {});
std::vector<DeviceBatteryInfo> ReadAssociationEndpointBattery(const WindowsBatteryQueryReaderContext& context,
                                                               std::vector<EndpointCandidate>* tws_candidates,
                                                               const std::string& target_device_id = std::string(),
                                                               const ProviderOperationContext& operation = {});
std::vector<DeviceBatteryInfo> ReadGenericDeviceBattery(const WindowsBatteryQueryReaderContext& context,
                                                        const ProviderOperationContext& operation = {});

}  // namespace battery_monitor

