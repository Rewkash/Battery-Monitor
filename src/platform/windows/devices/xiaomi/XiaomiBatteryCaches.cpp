#include "platform/windows/devices/xiaomi/XiaomiBatteryCaches.h"

#include <chrono>
#include <mutex>
#include <utility>

#include "platform/windows/devices/xiaomi/ClassicBluetoothBatteryFallback.h"
#include "platform/windows/devices/xiaomi/XiaomiBatteryReadings.h"
#include "platform/windows/devices/xiaomi/XiaomiModeCache.h"

namespace battery_monitor {

namespace {

constexpr auto kClassicLiveReadInterval = std::chrono::seconds(30);

void LogDebug(XiaomiDebugLogFn debug_log, const std::string& message) {
    if (debug_log != nullptr) {
        debug_log(message);
    }
}

}  // namespace

XiaomiClassicBatteryCache::XiaomiClassicBatteryCache(bool persist_write_enabled,
                                                     bool persist_read_enabled,
                                                     std::filesystem::path cache_file,
                                                     int ttl_minutes,
                                                     bool debug_enabled,
                                                     XiaomiDebugLogFn debug_log)
    : persist_write_enabled_(persist_write_enabled),
      persist_read_enabled_(persist_read_enabled),
      cache_file_(std::move(cache_file)),
      ttl_minutes_(ttl_minutes),
      debug_enabled_(debug_enabled),
      debug_log_(debug_log) {}

XiaomiReadResult XiaomiClassicBatteryCache::Read(std::uint64_t address,
                                                  bool aggressive_retry,
                                                  std::size_t min_tws_components) {
    (void)aggressive_retry;
    (void)min_tws_components;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        auto found = cache_.find(address);
        const bool has_cached_result = found != cache_.end();
        if (const auto successful = last_successful_live_read_.find(address);
            has_cached_result && successful != last_successful_live_read_.end() &&
            now - successful->second < kClassicLiveReadInterval) {
            return found->second;
        }
        if (const auto in_progress = live_read_in_progress_.find(address);
            in_progress != live_read_in_progress_.end()) {
            LogDebug(debug_log_,
                     "Xiaomi classic fallback: skipping live read already in progress for address=" +
                         std::to_string(address));
            if (has_cached_result) {
                auto result = found->second;
                result.from_persistent_cache = true;
                return result;
            }
            return {};
        }
        if (const auto failed = last_failed_live_read_.find(address);
            failed != last_failed_live_read_.end() && now - failed->second < kClassicLiveReadInterval) {
            LogDebug(debug_log_,
                     "Xiaomi classic fallback: skipping recent failed live read for address=" +
                         std::to_string(address));
            return {};
        }

        live_read_in_progress_[address] = now;
    }

    XiaomiReadResult read_result;
    std::vector<BatteryReading> readings;
    try {
        readings = TryReadXiaomiClassicBattery(
            address,
            &PutXiaomiModeCacheEntry,
            debug_enabled_,
            debug_log_);
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        live_read_in_progress_.erase(address);
        last_failed_live_read_[address] = std::chrono::steady_clock::now();
        throw;
    }

    if (!readings.empty()) {
        read_result.readings = readings;
        read_result.from_persistent_cache = false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    live_read_in_progress_.erase(address);
    if (!readings.empty()) {
        last_failed_live_read_.erase(address);
        last_successful_live_read_[address] = std::chrono::steady_clock::now();
    } else {
        last_failed_live_read_[address] = std::chrono::steady_clock::now();
    }

    auto found = cache_.find(address);
    const bool has_cached_result = found != cache_.end();
    if (has_cached_result) {
        if (read_result.readings.empty()) {
            LogDebug(debug_log_,
                     "Xiaomi classic fallback: live read failed; stale snapshot suppressed for address=" +
                         std::to_string(address));
            return {};
        }
        found->second = std::move(read_result);
        return found->second;
    }

    if (!read_result.readings.empty()) {
        auto inserted = cache_.emplace(address, read_result);
        return inserted.first->second;
    }
    return read_result;
}

void XiaomiClassicBatteryCache::Persist(std::uint64_t address, const std::vector<BatteryReading>& readings) const {
    (void)address;
    (void)readings;
}

}  // namespace battery_monitor

