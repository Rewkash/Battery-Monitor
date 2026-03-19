#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "platform/windows/devices/xiaomi/XiaomiHandshake.h"

namespace battery_monitor {

struct PnpBluetoothVisualHints {
    std::optional<std::uint32_t> bluetooth_cod_major;
    std::optional<std::uint32_t> bluetooth_cod_minor;
    std::vector<std::string> device_categories;
};

std::optional<PnpBluetoothVisualHints> ReadBluetoothVisualHintsFromPnpAddress(std::uint64_t address);
std::optional<std::uint8_t> ReadPhoneHfpBatteryHintFromPnpAddress(std::uint64_t address,
                                                                  bool debug_enabled = false,
                                                                  XiaomiDebugLogFn debug_log = nullptr);

}  // namespace battery_monitor

