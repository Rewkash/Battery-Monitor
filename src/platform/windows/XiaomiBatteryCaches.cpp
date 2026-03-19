#include "platform/windows/XiaomiBatteryCaches.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>

#include "platform/windows/ClassicBluetoothBatteryFallback.h"
#include "platform/windows/XiaomiBatteryReadings.h"
#include "platform/windows/XiaomiModeCache.h"
#include "platform/windows/XiaomiPersistentCache.h"

namespace battery_monitor {

namespace {

std::int64_t CurrentUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
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
                                                     int zmi_observe_ms,
                                                     bool debug_enabled,
                                                     XiaomiDebugLogFn debug_log)
    : persist_write_enabled_(persist_write_enabled),
      persist_read_enabled_(persist_read_enabled),
      cache_file_(std::move(cache_file)),
      ttl_minutes_(ttl_minutes),
      zmi_observe_ms_(zmi_observe_ms),
      debug_enabled_(debug_enabled),
      debug_log_(debug_log) {}

const XiaomiReadResult& XiaomiClassicBatteryCache::Read(std::uint64_t address,
                                                        bool aggressive_retry,
                                                        bool enable_dynamic_port_scan,
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
        enable_dynamic_port_scan,
        &PutXiaomiModeCacheEntry,
        zmi_observe_ms_,
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
            enable_dynamic_port_scan,
            &PutXiaomiModeCacheEntry,
            zmi_observe_ms_,
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
            enable_dynamic_port_scan,
            &PutXiaomiModeCacheEntry,
            zmi_observe_ms_,
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

XiaomiAdvertisementBatteryCache::XiaomiAdvertisementBatteryCache(int advertisement_scan_ms,
                                                                 int observe_ms,
                                                                 OpenBleDeviceByAddressFn open_ble_device,
                                                                 bool debug_enabled,
                                                                 XiaomiDebugLogFn debug_log)
    : advertisement_scan_ms_(advertisement_scan_ms),
      observe_ms_(observe_ms),
      open_ble_device_(open_ble_device),
      debug_enabled_(debug_enabled),
      debug_log_(debug_log) {}

std::vector<BatteryReading> XiaomiAdvertisementBatteryCache::Read(std::uint64_t address,
                                                                  const std::string& device_name_hint,
                                                                  bool prefer_extended_scan) {
    if (address <= 0xFFFFULL && device_name_hint.empty()) {
        return {};
    }

    int requested_scan_ms = advertisement_scan_ms_;
    if (prefer_extended_scan) {
        if (observe_ms_ > 0) {
            requested_scan_ms = std::max(requested_scan_ms, std::min(observe_ms_, 20000));
        } else {
            requested_scan_ms = std::max(requested_scan_ms, 3200);
        }
    }

    const auto cache_has_hit = [&]() {
        if (address > 0xFFFFULL && snapshot_cache_.by_address.find(address) != snapshot_cache_.by_address.end()) {
            return true;
        }

        if (!device_name_hint.empty()) {
            const std::string normalized_hint = ToLowerAscii(device_name_hint);
            if (snapshot_cache_.by_name.find(normalized_hint) != snapshot_cache_.by_name.end()) {
                return true;
            }
            for (const auto& [name, snapshot] : snapshot_cache_.by_name) {
                (void)snapshot;
                if (!name.empty() &&
                    (name.find(normalized_hint) != std::string::npos ||
                     normalized_hint.find(name) != std::string::npos)) {
                    return true;
                }
            }
        }

        if (address > 0xFFFFULL) {
            const auto expected_product_id = ReadBluetoothProductIdFromRegistry(address);
            if (expected_product_id.has_value() &&
                snapshot_cache_.by_product_id.find(*expected_product_id) != snapshot_cache_.by_product_id.end()) {
                return true;
            }
        }

        return false;
    };

    auto snapshot_score = [](const XiaomiBatterySnapshot& snapshot) {
        return XiaomiReadingsRichnessScore(BuildXiaomiBatteryReadings(snapshot));
    };
    auto merge_snapshot_map = [&](auto* target, const auto& source) {
        if (target == nullptr) {
            return;
        }
        for (const auto& [key, snapshot] : source) {
            const auto found = target->find(key);
            if (found == target->end() ||
                snapshot_score(snapshot) > snapshot_score(found->second)) {
                (*target)[key] = snapshot;
            }
        }
    };
    auto scan_and_merge = [&](int scan_ms, const char* phase_tag) {
        if (scan_ms <= 0) {
            return;
        }
        const auto scan_started_at = std::chrono::steady_clock::now();
        auto scanned_result = ScanXiaomiAdvertisementSnapshots(
            std::chrono::milliseconds(scan_ms),
            open_ble_device_,
            debug_enabled_,
            debug_log_);

        merge_snapshot_map(&snapshot_cache_.by_address, scanned_result.by_address);
        merge_snapshot_map(&snapshot_cache_.by_name, scanned_result.by_name);
        merge_snapshot_map(&snapshot_cache_.by_product_id, scanned_result.by_product_id);

        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - scan_started_at);
        LogDebug(debug_log_,
                 std::string("BLE advertisement fallback ") + phase_tag + " in " +
                     std::to_string(elapsed_ms.count()) +
                     " ms (requested=" + std::to_string(scan_ms) + ")" +
                     ", candidate addresses=" + std::to_string(snapshot_cache_.by_address.size()) +
                     " names=" + std::to_string(snapshot_cache_.by_name.size()));
    };
    auto resolve_readings_from_snapshot_cache = [&]() -> std::vector<BatteryReading> {
        const auto found = snapshot_cache_.by_address.find(address);
        if (found != snapshot_cache_.by_address.end()) {
            auto readings = BuildXiaomiBatteryReadings(found->second);
            if (debug_enabled_) {
                std::string components;
                for (const auto& reading : readings) {
                    if (!components.empty()) {
                        components += ",";
                    }
                    components += reading.component + ":" + std::to_string(reading.percent);
                }
                LogDebug(debug_log_,
                         "BLE advertisement fallback hit address=" + std::to_string(address) +
                             " entries=" + std::to_string(readings.size()) +
                             (components.empty() ? "" : " values=" + components));
            }
            return readings;
        }

        if (!device_name_hint.empty()) {
            const std::string normalized_hint = ToLowerAscii(device_name_hint);
            auto by_name = snapshot_cache_.by_name.find(normalized_hint);
            if (by_name == snapshot_cache_.by_name.end()) {
                for (const auto& [name, snapshot] : snapshot_cache_.by_name) {
                    (void)snapshot;
                    if (!name.empty() &&
                        (name.find(normalized_hint) != std::string::npos ||
                         normalized_hint.find(name) != std::string::npos)) {
                        by_name = snapshot_cache_.by_name.find(name);
                        break;
                    }
                }
            }
            if (by_name != snapshot_cache_.by_name.end()) {
                auto readings = BuildXiaomiBatteryReadings(by_name->second);
                if (debug_enabled_) {
                    LogDebug(debug_log_,
                             "BLE advertisement fallback hit by name='" + normalized_hint +
                                 "' entries=" + std::to_string(readings.size()));
                }
                return readings;
            }
        }

        const auto expected_product_id = ReadBluetoothProductIdFromRegistry(address);
        if (expected_product_id.has_value()) {
            const auto by_pid = snapshot_cache_.by_product_id.find(*expected_product_id);
            if (by_pid != snapshot_cache_.by_product_id.end()) {
                auto readings = BuildXiaomiBatteryReadings(by_pid->second);
                if (debug_enabled_) {
                    std::ostringstream pid_stream;
                    pid_stream << "0x" << std::uppercase << std::hex
                               << std::setw(4) << std::setfill('0') << *expected_product_id;
                    LogDebug(debug_log_,
                             "BLE advertisement fallback hit by productId=" + pid_stream.str() +
                                 " entries=" + std::to_string(readings.size()));
                }
                return readings;
            }
        }

        return {};
    };

    if (!scan_attempted_ || (!cache_has_hit() && requested_scan_ms > scan_budget_ms_ + 250)) {
        scan_attempted_ = true;
        scan_budget_ms_ = std::max(scan_budget_ms_, requested_scan_ms);
        scan_and_merge(requested_scan_ms, "scanned");
    }

    auto readings = resolve_readings_from_snapshot_cache();
    if (!readings.empty()) {
        return readings;
    }

    if (prefer_extended_scan && !rescan_attempted_ && !cache_has_hit()) {
        rescan_attempted_ = true;
        const int rescan_ms =
            requested_scan_ms >= 16000 ? 4200 :
            (requested_scan_ms >= 10000 ? 3200 :
             std::clamp(requested_scan_ms + 2200, 2200, 12000));
        scan_budget_ms_ = std::max(scan_budget_ms_, rescan_ms);
        scan_and_merge(rescan_ms, "rescanned");
        readings = resolve_readings_from_snapshot_cache();
        if (!readings.empty()) {
            return readings;
        }
    }

    return {};
}

}  // namespace battery_monitor
