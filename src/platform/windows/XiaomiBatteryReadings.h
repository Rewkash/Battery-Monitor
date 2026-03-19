#pragma once

#include <cstddef>
#include <vector>

#include "platform/windows/XiaomiBatteryCodec.h"

namespace battery_monitor {

std::size_t XiaomiResolvedTwsComponentCount(const std::vector<BatteryReading>& readings);
bool HasUsefulXiaomiTwsReadings(const std::vector<BatteryReading>& readings,
                                std::size_t min_components = 2U);
int XiaomiReadingsRichnessScore(const std::vector<BatteryReading>& readings);

}  // namespace battery_monitor
