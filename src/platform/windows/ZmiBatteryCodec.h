#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "platform/windows/XiaomiBatteryCodec.h"

namespace battery_monitor {

std::optional<std::uint8_t> NormalizeZmiVendorBatteryScalar(int raw_value);
std::optional<XiaomiBatterySnapshot> ExtractZmiSerialPatternSnapshot(
    const std::vector<std::uint8_t>& bytes);
std::optional<XiaomiBatterySnapshot> ExtractZmiSerialTextSnapshot(const std::string& text);

}  // namespace battery_monitor
