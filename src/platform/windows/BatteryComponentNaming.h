#pragma once

#include <string>
#include <vector>

#include "platform/windows/XiaomiBatteryCodec.h"

namespace battery_monitor {

std::string NormalizeBatteryComponentHint(const std::string& hint);
void AssignFallbackBatteryComponents(std::vector<BatteryReading>* readings, bool prefer_tws_labels);
int BatteryComponentSortWeight(const std::string& component);

}  // namespace battery_monitor
