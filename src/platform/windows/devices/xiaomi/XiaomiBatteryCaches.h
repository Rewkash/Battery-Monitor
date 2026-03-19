#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "platform/windows/devices/xiaomi/XiaomiBatteryCodec.h"
#include "platform/windows/devices/xiaomi/XiaomiHandshake.h"

namespace battery_monitor {

struct XiaomiReadResult {
    std::vector<BatteryReading> readings;
    bool from_persistent_cache = false;
};

class XiaomiClassicBatteryCache {
   public:
    XiaomiClassicBatteryCache(bool persist_write_enabled,
                              bool persist_read_enabled,
                              std::filesystem::path cache_file,
                              int ttl_minutes,
                              bool debug_enabled = false,
                              XiaomiDebugLogFn debug_log = nullptr);

    const XiaomiReadResult& Read(std::uint64_t address,
                                 bool aggressive_retry,
                                 std::size_t min_tws_components = 1U);
    XiaomiReadResult ReadPersistent(std::uint64_t address, std::size_t min_tws_components = 1U) const;
    void Persist(std::uint64_t address, const std::vector<BatteryReading>& readings) const;

   private:
    bool persist_write_enabled_ = true;
    bool persist_read_enabled_ = true;
    std::filesystem::path cache_file_;
    int ttl_minutes_ = 180;
    bool debug_enabled_ = false;
    XiaomiDebugLogFn debug_log_ = nullptr;
    std::unordered_map<std::uint64_t, XiaomiReadResult> cache_;
};

}  // namespace battery_monitor

