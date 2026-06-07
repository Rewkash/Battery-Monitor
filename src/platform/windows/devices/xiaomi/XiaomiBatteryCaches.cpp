#include "platform/windows/devices/xiaomi/XiaomiBatteryCaches.h"

#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

#include "platform/windows/devices/xiaomi/ClassicBluetoothBatteryFallback.h"
#include "platform/windows/devices/xiaomi/XiaomiBatteryReadings.h"
#include "platform/windows/devices/xiaomi/XiaomiModeCache.h"

namespace battery_monitor {

namespace {

constexpr auto kFailedClassicLiveReadBackoff = std::chrono::seconds(10);

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

const XiaomiReadResult& XiaomiClassicBatteryCache::Read(std::uint64_t address,
                                                         bool aggressive_retry,
                                                         std::size_t min_tws_components) {
    const auto now = std::chrono::steady_clock::now();
    const auto is_sufficient = [&](const std::vector<BatteryReading>& candidate_readings) {
        if (candidate_readings.empty()) {
            return false;
        }
        if (min_tws_components <= 1U) {
            return true;
        }
        return XiaomiResolvedTwsComponentCount(candidate_readings) >= min_tws_components;
    };

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = cache_.find(address);
        const bool has_cached_result = found != cache_.end();
        if (has_cached_result && is_sufficient(found->second.readings)) {
            return found->second;
        }
        if (const auto in_progress = live_read_in_progress_.find(address);
            in_progress != live_read_in_progress_.end()) {
            LogDebug(debug_log_,
                     "Xiaomi classic fallback: skipping live read already in progress for address=" +
                         std::to_string(address));
            if (has_cached_result) {
                return found->second;
            }
            auto inserted = cache_.emplace(address, XiaomiReadResult{});
            return inserted.first->second;
        }
        if (const auto failed = last_failed_live_read_.find(address);
            failed != last_failed_live_read_.end() && now - failed->second < kFailedClassicLiveReadBackoff) {
            LogDebug(debug_log_,
                     "Xiaomi classic fallback: skipping recent failed live read for address=" +
                         std::to_string(address));
            if (has_cached_result) {
                return found->second;
            }
            auto inserted = cache_.emplace(address, XiaomiReadResult{});
            return inserted.first->second;
        }

        live_read_in_progress_[address] = now;
    }

    XiaomiReadResult read_result;
    auto readings = TryReadXiaomiClassicBattery(
        address,
        &PutXiaomiModeCacheEntry,
        debug_enabled_,
        debug_log_);
    if (aggressive_retry && !readings.empty() && !is_sufficient(readings)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
        auto retried = TryReadXiaomiClassicBattery(
            address,
            &PutXiaomiModeCacheEntry,
            debug_enabled_,
            debug_log_);
        if (XiaomiReadingsRichnessScore(retried) > XiaomiReadingsRichnessScore(readings)) {
            readings = std::move(retried);
        }
    }
    if (aggressive_retry && !readings.empty() && !is_sufficient(readings)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(260));
        auto retried = TryReadXiaomiClassicBattery(
            address,
            &PutXiaomiModeCacheEntry,
            debug_enabled_,
            debug_log_);
        if (XiaomiReadingsRichnessScore(retried) > XiaomiReadingsRichnessScore(readings)) {
            readings = std::move(retried);
        }
    }

    if (!readings.empty()) {
        read_result.readings = readings;
        read_result.from_persistent_cache = false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    live_read_in_progress_.erase(address);
    if (!readings.empty()) {
        last_failed_live_read_.erase(address);
    } else {
        last_failed_live_read_[address] = std::chrono::steady_clock::now();
    }

    auto found = cache_.find(address);
    const bool has_cached_result = found != cache_.end();
    if (has_cached_result) {
        if (XiaomiReadingsRichnessScore(found->second.readings) >
            XiaomiReadingsRichnessScore(read_result.readings)) {
            found->second.from_persistent_cache = true;
            LogDebug(debug_log_,
                     "Xiaomi classic fallback: using last successful live snapshot for address=" +
                         std::to_string(address));
            return found->second;
        }
        found->second = std::move(read_result);
        return found->second;
    }

    auto inserted = cache_.emplace(address, std::move(read_result));
    return inserted.first->second;
}

XiaomiReadResult XiaomiClassicBatteryCache::ReadPersistent(std::uint64_t address,
                                                           std::size_t min_tws_components) const {
    XiaomiReadResult result;
    const auto found = cache_.find(address);
    if (found == cache_.end()) {
        return result;
    }

    const auto& readings = found->second.readings;
    if (readings.empty()) {
        return result;
    }
    if (min_tws_components > 1U &&
        XiaomiResolvedTwsComponentCount(readings) < min_tws_components) {
        return result;
    }

    result.readings = readings;
    result.from_persistent_cache = true;
    return result;
}

void XiaomiClassicBatteryCache::Persist(std::uint64_t address, const std::vector<BatteryReading>& readings) const {
    (void)address;
    (void)readings;
}

}  // namespace battery_monitor

