#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "platform/windows/devices/xiaomi/XiaomiBatteryCodec.h"
#include "platform/windows/devices/xiaomi/ClassicBluetoothBatteryFallback.h"
#include "platform/windows/devices/xiaomi/XiaomiHandshake.h"

namespace battery_monitor {

class XiaomiRfcommSessionManager;

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
                               XiaomiRfcommSessionManager* session_manager,
                               bool debug_enabled = false,
                              XiaomiDebugLogFn debug_log = nullptr);

    XiaomiReadResult Read(std::uint64_t address,
                          ClassicBatteryService preferred_service,
                          bool aggressive_retry,
                          std::size_t min_tws_components = 1U);
    void Persist(std::uint64_t address, const std::vector<BatteryReading>& readings) const;

   private:
    bool persist_write_enabled_ = true;
    bool persist_read_enabled_ = true;
    std::filesystem::path cache_file_;
    int ttl_minutes_ = 180;
    bool debug_enabled_ = false;
    XiaomiDebugLogFn debug_log_ = nullptr;
    XiaomiRfcommSessionManager* session_manager_ = nullptr;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, XiaomiReadResult> cache_;
    std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> last_successful_live_read_;
    std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> last_failed_live_read_;
    std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> live_read_in_progress_;
    std::unordered_map<std::uint64_t, bool> services_exhausted_;
};

}  // namespace battery_monitor

