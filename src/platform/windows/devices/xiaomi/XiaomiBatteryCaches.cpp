#include "platform/windows/devices/xiaomi/XiaomiBatteryCaches.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <utility>

#include "platform/windows/devices/xiaomi/ClassicBluetoothBatteryFallback.h"
#include "platform/windows/devices/xiaomi/XiaomiBatteryReadings.h"
#include "platform/windows/devices/xiaomi/XiaomiModeCache.h"
#include "platform/windows/devices/xiaomi/XiaomiRfcommSessionManager.h"

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
                                                      XiaomiRfcommSessionManager* session_manager,
                                                      bool debug_enabled,
                                                     XiaomiDebugLogFn debug_log)
    : persist_write_enabled_(persist_write_enabled),
      persist_read_enabled_(persist_read_enabled),
      cache_file_(std::move(cache_file)),
      ttl_minutes_(ttl_minutes),
      debug_enabled_(debug_enabled),
      debug_log_(debug_log),
      session_manager_(session_manager) {}

XiaomiReadResult XiaomiClassicBatteryCache::Read(std::uint64_t address,
                                                   ClassicBatteryService preferred_service,
                                                   bool aggressive_retry,
                                                   std::size_t min_tws_components,
                                                   const ProviderOperationContext& operation) {
    (void)min_tws_components;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        auto found = cache_.find(address);
        const bool has_cached_result = found != cache_.end();
        const bool push_data_available = push_data_available_.contains(address);
        if (preferred_service == ClassicBatteryService::kZmiPurPodsSerial && has_cached_result &&
            !last_failed_live_read_.contains(address) && !aggressive_retry && !push_data_available) {
            LogDebug(debug_log_,
                     "ZMI classic fallback: reusing session cache without another RFCOMM connection address=" +
                         std::to_string(address));
            auto result = found->second;
            result.from_persistent_cache = true;
            return result;
        }
        if (preferred_service == ClassicBatteryService::kZmiPurPodsSerial &&
            services_exhausted_[address] && !aggressive_retry && !push_data_available) {
            LogDebug(debug_log_,
                     "Xiaomi classic fallback: all RFCOMM services already failed for current connection address=" +
                         std::to_string(address));
            return {};
        }
        if (const auto successful = last_successful_live_read_.find(address);
            !aggressive_retry && !push_data_available && has_cached_result &&
            successful != last_successful_live_read_.end() &&
            now - successful->second < kClassicLiveReadInterval) {
            auto result = found->second;
            result.from_persistent_cache = true;
            return result;
        }
        if (const auto in_progress = live_read_in_progress_.find(address);
            in_progress != live_read_in_progress_.end()) {
            LogDebug(debug_log_,
                     "Xiaomi classic fallback: skipping live read already in progress for address=" +
                         std::to_string(address));
            return {};
        }
        if (const auto failed = last_failed_live_read_.find(address);
            !aggressive_retry && !push_data_available && failed != last_failed_live_read_.end() &&
            now - failed->second < kClassicLiveReadInterval) {
            if (has_cached_result) {
                LogDebug(debug_log_,
                         "Xiaomi classic fallback: reusing cached snapshot after recent failed live read for address=" +
                             std::to_string(address));
                auto result = found->second;
                result.from_persistent_cache = true;
                return result;
            }
            return {};
        }

        live_read_in_progress_[address] = now;
        push_data_available_.erase(address);
    }

    XiaomiReadResult read_result;
    std::vector<BatteryReading> readings;
    std::optional<ClassicBatteryService> connected_service;
    try {
        if (session_manager_ != nullptr) {
            readings = session_manager_->ReadBattery(address, preferred_service, operation);
            if (!readings.empty()) {
                connected_service = TryGetSuccessfulClassicBatteryService(address);
            }
        } else {
            std::vector<ClassicBatteryService> services;
            const auto add_service = [&](ClassicBatteryService service) {
                if (std::find(services.begin(), services.end(), service) == services.end()) {
                    services.push_back(service);
                }
            };
            if (preferred_service == ClassicBatteryService::kZmiPurPodsSerial) {
                add_service(ClassicBatteryService::kZmiPurPodsSerial);
            } else {
                if (const auto known = TryGetSuccessfulClassicBatteryService(address); known.has_value()) {
                    add_service(*known);
                }
                add_service(preferred_service);
                add_service(ClassicBatteryService::kBluetoothSerialPort);
                add_service(ClassicBatteryService::kZmiPurPodsSerial);
            }

            for (std::size_t index = 0; index < services.size(); ++index) {
                if (operation.IsCancelled()) break;
                readings = TryReadXiaomiClassicBattery(address,
                                                       services[index],
                                                       nullptr,
                                                       &PutXiaomiModeCacheEntry,
                                                       debug_enabled_,
                                                       debug_log_);
                if (!readings.empty()) {
                    connected_service = services[index];
                    break;
                }
            }
        }
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
    if (operation.IsCancelled()) {
        return read_result;
    }
    if (connected_service.has_value()) {
        RememberSuccessfulClassicBatteryService(address, *connected_service);
        services_exhausted_.erase(address);
    } else if (preferred_service == ClassicBatteryService::kZmiPurPodsSerial) {
        services_exhausted_[address] = true;
    }
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
                     "Xiaomi classic fallback: live read failed; reusing cached snapshot for address=" +
                         std::to_string(address));
            auto result = found->second;
            result.from_persistent_cache = true;
            return result;
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

void XiaomiClassicBatteryCache::NotifyPushDataAvailable(std::uint64_t address) {
    std::lock_guard<std::mutex> lock(mutex_);
    push_data_available_.insert(address);
}

void XiaomiClassicBatteryCache::Persist(std::uint64_t address, const std::vector<BatteryReading>& readings) const {
    (void)address;
    (void)readings;
}

}  // namespace battery_monitor

