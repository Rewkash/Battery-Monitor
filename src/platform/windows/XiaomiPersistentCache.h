#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "platform/windows/XiaomiBatteryCodec.h"

namespace battery_monitor {

XiaomiBatterySnapshot SnapshotFromBatteryReadings(const std::vector<BatteryReading>& readings);
void PutPersistentXiaomiSnapshot(std::uint64_t address,
                                 const XiaomiBatterySnapshot& snapshot,
                                 const std::filesystem::path& cache_file,
                                 std::int64_t now_unix);
std::optional<XiaomiBatterySnapshot> GetPersistentXiaomiSnapshot(std::uint64_t address,
                                                                 const std::filesystem::path& cache_file,
                                                                 std::int64_t now_unix,
                                                                 int ttl_minutes);

}  // namespace battery_monitor
