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
    void RemoveTwsBatteryEntriesForAddress(std::uint64_t address,
                                           const std::unordered_set<std::string>& components);
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
                                                                       XiaomiDebugLogFn debug_log = nullptr,
                                                                       const ProviderOperationContext& operation = {});
void ApplyPnpVisualHints(std::vector<DeviceBatteryInfo>* entries);
// Deterministic source priority for live readings within one snapshot.
//
// Readers run in a fixed stage order (see WinRtBatteryProvider::GetDevicesBattery):
//   1. TWS stage      - dedicated/vendor readers (Xiaomi classic RFCOMM, endpoint AEP
//                       properties) => highest priority, appended first.
//   2. BLE stage      - standard BLE battery service plus vendor fallbacks.
//   3. Generic/query and disconnected stages - PnP-queried or cached values, lowest.
//
// Because entries are appended in that order, the first writer for a given
// (address, component) is always the highest-priority source. Later duplicates
// with equal or lower priority never overwrite it (no last-writer-wins); the
// value conflict is reported through `debug_log` when provided.
int ReadingSourcePriority(const DeviceBatteryInfo& entry);

std::vector<DeviceBatteryInfo> FinalizeCollectedEntries(std::vector<DeviceBatteryInfo> entries,
                                                        bool include_disconnected,
                                                        const PairedBluetoothSnapshot* paired_snapshot = nullptr,
                                                        XiaomiDebugLogFn debug_log = nullptr);

}  // namespace battery_monitor

