#include "platform/windows/devices/xiaomi/XiaomiBatteryCaches.h"

#include <chrono>
#include <thread>
#include <utility>

#include "platform/windows/devices/xiaomi/ClassicBluetoothBatteryFallback.h"
#include "platform/windows/devices/xiaomi/XiaomiBatteryReadings.h"
#include "platform/windows/devices/xiaomi/XiaomiModeCache.h"
#include "platform/windows/devices/xiaomi/XiaomiPersistentCache.h"

namespace battery_monitor {

namespace {

std::int64_t CurrentUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

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
    auto found = cache_.find(address);
    const bool has_cached_result = found != cache_.end();
    const bool cached_is_sufficient =
        has_cached_result &&
        (!found->second.readings.empty()) &&
        (min_tws_components <= 1U ||
         XiaomiResolvedTwsComponentCount(found->second.readings) >= min_tws_components);
    if (cached_is_sufficient) {
        return found->second;
    }

    XiaomiReadResult read_result;
    auto readings = TryReadXiaomiClassicBattery(
        address,
        &PutXiaomiModeCacheEntry,
        debug_enabled_,
        debug_log_);
    const auto is_sufficient = [&](const std::vector<BatteryReading>& candidate_readings) {
        if (candidate_readings.empty()) {
            return false;
        }
        if (min_tws_components <= 1U) {
            return true;
        }
        return XiaomiResolvedTwsComponentCount(candidate_readings) >= min_tws_components;
    };

    if (aggressive_retry && !is_sufficient(readings)) {
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
    if (aggressive_retry && !is_sufficient(readings)) {
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
        if (persist_write_enabled_ && HasUsefulXiaomiTwsReadings(readings, 2U)) {
            PutPersistentXiaomiSnapshot(
                address,
                SnapshotFromBatteryReadings(readings),
                cache_file_,
                CurrentUnixSeconds());
        }
    }

    if (persist_read_enabled_) {
        const auto persistent_snapshot = GetPersistentXiaomiSnapshot(
            address, cache_file_, CurrentUnixSeconds(), ttl_minutes_);
        if (persistent_snapshot.has_value()) {
            auto persistent_readings = BuildXiaomiBatteryReadings(*persistent_snapshot);
            if (!persistent_readings.empty()) {
                const bool live_sufficient = is_sufficient(read_result.readings);
                const bool persistent_sufficient = is_sufficient(persistent_readings);
                if ((!live_sufficient && persistent_sufficient) ||
                    (read_result.readings.empty() &&
                     XiaomiReadingsRichnessScore(persistent_readings) >
                         XiaomiReadingsRichnessScore(read_result.readings))) {
                    read_result.readings = std::move(persistent_readings);
                    read_result.from_persistent_cache = true;
                    LogDebug(debug_log_,
                             "Xiaomi classic fallback: using persisted cache for address=" +
                                 std::to_string(address));
                }
            }
        }
    }

    if (has_cached_result) {
        if (XiaomiReadingsRichnessScore(found->second.readings) >
            XiaomiReadingsRichnessScore(read_result.readings)) {
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
    if (!persist_read_enabled_) {
        return result;
    }

    const auto persistent_snapshot = GetPersistentXiaomiSnapshot(
        address, cache_file_, CurrentUnixSeconds(), ttl_minutes_);
    if (!persistent_snapshot.has_value()) {
        return result;
    }

    auto readings = BuildXiaomiBatteryReadings(*persistent_snapshot);
    if (readings.empty()) {
        return result;
    }
    if (min_tws_components > 1U &&
        XiaomiResolvedTwsComponentCount(readings) < min_tws_components) {
        return result;
    }

    result.readings = std::move(readings);
    result.from_persistent_cache = true;
    return result;
}

void XiaomiClassicBatteryCache::Persist(std::uint64_t address, const std::vector<BatteryReading>& readings) const {
    if (!persist_write_enabled_ || !HasUsefulXiaomiTwsReadings(readings, 2U)) {
        return;
    }

    PutPersistentXiaomiSnapshot(
        address,
        SnapshotFromBatteryReadings(readings),
        cache_file_,
        CurrentUnixSeconds());
}

}  // namespace battery_monitor

