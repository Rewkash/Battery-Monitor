#include "platform/windows/devices/xiaomi/XiaomiPersistentCache.h"

#include <array>
#include <cstdint>
#include <exception>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace battery_monitor {

namespace {

struct XiaomiPersistentCacheEntry {
    std::int64_t updated_at_unix = 0;
    XiaomiBatterySnapshot snapshot;
};

using XiaomiPersistentCacheMap = std::unordered_map<std::uint64_t, XiaomiPersistentCacheEntry>;

int SnapshotValueOrMissing(const std::optional<std::uint8_t>& value) {
    return value.has_value() ? static_cast<int>(*value) : -1;
}

std::optional<std::uint8_t> SnapshotValueFromInt(int value) {
    if (value < 0 || value > 100) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(value);
}

XiaomiPersistentCacheMap LoadPersistentXiaomiCache(const std::filesystem::path& cache_file) {
    XiaomiPersistentCacheMap cache;

    std::ifstream input(cache_file, std::ios::in);
    if (!input.is_open()) {
        return cache;
    }

    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }

        std::istringstream parser(line);
        std::string token;
        std::array<std::string, 5> fields{};
        bool parse_failed = false;
        for (auto& field : fields) {
            if (!std::getline(parser, token, '|')) {
                parse_failed = true;
                break;
            }
            field = token;
        }
        if (parse_failed) {
            continue;
        }

        try {
            const auto address = std::stoull(fields[0]);
            const auto updated = std::stoll(fields[1]);
            const int left = std::stoi(fields[2]);
            const int right = std::stoi(fields[3]);
            const int case_level = std::stoi(fields[4]);

            XiaomiPersistentCacheEntry entry;
            entry.updated_at_unix = updated;
            entry.snapshot.left = SnapshotValueFromInt(left);
            entry.snapshot.right = SnapshotValueFromInt(right);
            entry.snapshot.case_level = SnapshotValueFromInt(case_level);
            if (HasAnyBattery(entry.snapshot)) {
                cache[address] = entry;
            }
        } catch (const std::exception&) {
            continue;
        }
    }

    return cache;
}

void SavePersistentXiaomiCache(const std::filesystem::path& cache_file,
                               const XiaomiPersistentCacheMap& cache) {
    std::ofstream output(cache_file, std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        return;
    }

    for (const auto& [address, entry] : cache) {
        output << address << "|"
               << entry.updated_at_unix << "|"
               << SnapshotValueOrMissing(entry.snapshot.left) << "|"
               << SnapshotValueOrMissing(entry.snapshot.right) << "|"
               << SnapshotValueOrMissing(entry.snapshot.case_level) << "\n";
    }
}

XiaomiPersistentCacheMap& PersistentXiaomiCacheStore(const std::filesystem::path& cache_file) {
    static XiaomiPersistentCacheMap cache = LoadPersistentXiaomiCache(cache_file);
    return cache;
}

}  // namespace

XiaomiBatterySnapshot SnapshotFromBatteryReadings(const std::vector<BatteryReading>& readings) {
    XiaomiBatterySnapshot snapshot;
    for (const auto& reading : readings) {
        if (reading.component == "left") {
            snapshot.left = reading.percent;
        } else if (reading.component == "right") {
            snapshot.right = reading.percent;
        } else if (reading.component == "case") {
            snapshot.case_level = reading.percent;
        }
    }
    return snapshot;
}

void PutPersistentXiaomiSnapshot(std::uint64_t address,
                                 const XiaomiBatterySnapshot& snapshot,
                                 const std::filesystem::path& cache_file,
                                 std::int64_t now_unix) {
    if (!HasAnyBattery(snapshot)) {
        return;
    }

    auto& cache = PersistentXiaomiCacheStore(cache_file);
    XiaomiPersistentCacheEntry entry;
    entry.updated_at_unix = now_unix;
    entry.snapshot = snapshot;
    cache[address] = entry;
    SavePersistentXiaomiCache(cache_file, cache);
}

std::optional<XiaomiBatterySnapshot> GetPersistentXiaomiSnapshot(std::uint64_t address,
                                                                 const std::filesystem::path& cache_file,
                                                                 std::int64_t now_unix,
                                                                 int ttl_minutes) {
    auto& cache = PersistentXiaomiCacheStore(cache_file);
    const auto found = cache.find(address);
    if (found == cache.end()) {
        return std::nullopt;
    }

    const auto age_seconds = now_unix - found->second.updated_at_unix;
    const auto ttl_seconds = static_cast<std::int64_t>(ttl_minutes) * 60LL;
    if (age_seconds < 0 || age_seconds > ttl_seconds) {
        cache.erase(found);
        SavePersistentXiaomiCache(cache_file, cache);
        return std::nullopt;
    }

    if (!HasAnyBattery(found->second.snapshot)) {
        return std::nullopt;
    }

    return found->second.snapshot;
}

}  // namespace battery_monitor

