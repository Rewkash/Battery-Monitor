#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "platform/windows/WindowsBluetoothAddressUtils.h"
#include "platform/windows/XiaomiAdvertisementSnapshots.h"
#include "platform/windows/XiaomiBatteryCodec.h"
#include "platform/windows/XiaomiHandshake.h"

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
                              int zmi_observe_ms,
                              bool debug_enabled = false,
                              XiaomiDebugLogFn debug_log = nullptr);

    const XiaomiReadResult& Read(std::uint64_t address,
                                 bool aggressive_retry,
                                 bool enable_dynamic_port_scan,
                                 std::size_t min_tws_components = 1U);
    XiaomiReadResult ReadPersistent(std::uint64_t address, std::size_t min_tws_components = 1U) const;
    void Persist(std::uint64_t address, const std::vector<BatteryReading>& readings) const;

   private:
    bool persist_write_enabled_ = true;
    bool persist_read_enabled_ = true;
    std::filesystem::path cache_file_;
    int ttl_minutes_ = 180;
    int zmi_observe_ms_ = 0;
    bool debug_enabled_ = false;
    XiaomiDebugLogFn debug_log_ = nullptr;
    std::unordered_map<std::uint64_t, XiaomiReadResult> cache_;
};

class XiaomiAdvertisementBatteryCache {
   public:
    XiaomiAdvertisementBatteryCache(int advertisement_scan_ms,
                                    int observe_ms,
                                    OpenBleDeviceByAddressFn open_ble_device,
                                    bool debug_enabled = false,
                                    XiaomiDebugLogFn debug_log = nullptr);

    std::vector<BatteryReading> Read(std::uint64_t address,
                                     const std::string& device_name_hint,
                                     bool prefer_extended_scan = false);

   private:
    int advertisement_scan_ms_ = 1800;
    int observe_ms_ = 0;
    OpenBleDeviceByAddressFn open_ble_device_ = nullptr;
    bool debug_enabled_ = false;
    XiaomiDebugLogFn debug_log_ = nullptr;
    AdvertisementSnapshotResult snapshot_cache_;
    bool scan_attempted_ = false;
    bool rescan_attempted_ = false;
    int scan_budget_ms_ = 0;
};

}  // namespace battery_monitor
