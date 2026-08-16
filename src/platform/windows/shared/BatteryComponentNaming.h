#pragma once

#include <string>
#include <vector>

#include "platform/windows/devices/xiaomi/XiaomiBatteryCodec.h"
#include "platform/windows/devices/xiaomi/XiaomiHandshake.h"

namespace battery_monitor {

std::string NormalizeBatteryComponentHint(const std::string& hint);

// Assigns left/right/case (or partN) labels to readings without an explicit
// component. Ordering is made deterministic within the snapshot before
// assignment: readings that already carry a component keep their label and
// come first (by BatteryComponentSortWeight); unlabeled readings are ordered
// by descending battery level, so the same set of readings always maps to the
// same left/right/case assignment regardless of reader completion order.
// Heuristic (fallback) assignments are reported through `debug_log` when
// provided, tagged as origin "heuristic".
void AssignFallbackBatteryComponents(std::vector<BatteryReading>* readings,
                                      bool prefer_tws_labels,
                                      XiaomiDebugLogFn debug_log = nullptr,
                                      const std::string& debug_context = {});
int BatteryComponentSortWeight(const std::string& component);

}  // namespace battery_monitor

