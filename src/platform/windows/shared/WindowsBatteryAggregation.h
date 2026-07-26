#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/BatteryTypes.h"
#include "platform/windows/devices/xiaomi/XiaomiHandshake.h"

namespace battery_monitor {

struct PairedBluetoothSnapshot {
    std::unordered_set<std::string> device_ids;
    std::unordered_set<std::uint64_t> addresses;
    bool loaded = false;
};

struct DisconnectedPairedCollection {
    PairedBluetoothSnapshot snapshot;
    std::vector<DeviceBatteryInfo> offline_entries;
};

class DeviceBatteryAccumulator {
   public:
    void AddEntry(DeviceBatteryInfo entry);
    void RemoveTwsBatteryEntriesForAddress(std::uint64_t address);
    void MarkAddressWithRealBattery(std::uint64_t address);
    bool Empty() const;
    const std::vector<DeviceBatteryInfo>& Entries() const;
    const std::unordered_set<std::uint64_t>& AddressesWithRealBattery() const;
    std::vector<DeviceBatteryInfo> TakeEntries();

   private:
    std::vector<DeviceBatteryInfo> entries_;
    std::unordered_set<std::uint64_t> addresses_with_real_battery_;
    std::unordered_map<std::string, std::size_t> known_entries_;
};

DisconnectedPairedCollection CollectDisconnectedPairedBluetoothEntries(bool debug_enabled = false,
                                                                      XiaomiDebugLogFn debug_log = nullptr);
void ApplyPnpVisualHints(std::vector<DeviceBatteryInfo>* entries);
std::vector<DeviceBatteryInfo> FinalizeCollectedEntries(std::vector<DeviceBatteryInfo> entries,
                                                        bool include_disconnected,
                                                        const PairedBluetoothSnapshot* paired_snapshot = nullptr);

}  // namespace battery_monitor

