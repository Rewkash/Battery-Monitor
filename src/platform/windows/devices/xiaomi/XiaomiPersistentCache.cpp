#include "platform/windows/devices/xiaomi/XiaomiPersistentCache.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <exception>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>

namespace battery_monitor {

namespace {

struct XiaomiPersistentCacheEntry {
    std::int64_t updated_at_unix = 0;
    XiaomiBatterySnapshot snapshot;
};

using XiaomiPersistentCacheMap = std::unordered_map<std::uint64_t, XiaomiPersistentCacheEntry>;

struct XiaomiPersistentCacheState {
    bool loaded = false;
    XiaomiPersistentCacheMap entries;
};

using XiaomiPersistentCacheStores = std::unordered_map<std::wstring, XiaomiPersistentCacheState>;

int SnapshotValueOrMissing(const std::optional<std::uint8_t>& value) {
    return value.has_value() ? static_cast<int>(*value) : -1;
}

std::optional<std::uint8_t> SnapshotValueFromInt(int value) {
    if (value < 0 || value > 100) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(value);
}

std::optional<XiaomiPersistentCacheMap> LoadPersistentXiaomiCache(const std::filesystem::path& cache_file) {
    XiaomiPersistentCacheMap cache;

    std::ifstream input(cache_file, std::ios::in);
    if (!input.is_open()) {
        std::error_code error;
        const bool exists = std::filesystem::exists(cache_file, error);
        return !error && !exists ? std::optional<XiaomiPersistentCacheMap>{std::move(cache)} : std::nullopt;
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
            const auto address = static_cast<std::uint64_t>(std::stoull(fields[0]));
            XiaomiPersistentCacheEntry entry;
            entry.updated_at_unix = std::stoll(fields[1]);
            entry.snapshot.left = SnapshotValueFromInt(std::stoi(fields[2]));
            entry.snapshot.right = SnapshotValueFromInt(std::stoi(fields[3]));
            entry.snapshot.case_level = SnapshotValueFromInt(std::stoi(fields[4]));
            if (address != 0U && HasAnyBattery(entry.snapshot)) {
                cache[address] = entry;
            }
        } catch (const std::exception&) {
            continue;
        }
    }

    if (input.bad()) {
        return std::nullopt;
    }
    return cache;
}

bool SavePersistentXiaomiCache(const std::filesystem::path& cache_file, const XiaomiPersistentCacheMap& cache) {
    std::ostringstream serialized;
    for (const auto& [address, entry] : cache) {
        serialized << address << '|' << entry.updated_at_unix << '|' << SnapshotValueOrMissing(entry.snapshot.left)
                   << '|' << SnapshotValueOrMissing(entry.snapshot.right) << '|'
                   << SnapshotValueOrMissing(entry.snapshot.case_level) << '\n';
    }
    if (!serialized) {
        return false;
    }

    std::error_code error;
    const auto parent = cache_file.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return false;
        }
    }

    std::filesystem::path temporary;
    HANDLE output = INVALID_HANDLE_VALUE;
    for (unsigned int attempt = 0; attempt < 100U; ++attempt) {
        temporary = cache_file.wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
                    std::to_wstring(attempt);
        output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output != INVALID_HANDLE_VALUE) {
            break;
        }
        const DWORD create_error = GetLastError();
        if (create_error != ERROR_FILE_EXISTS && create_error != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    if (output == INVALID_HANDLE_VALUE) {
        return false;
    }

    const std::string bytes = serialized.str();
    std::size_t written_total = 0;
    bool write_succeeded = true;
    while (written_total < bytes.size()) {
        const DWORD requested = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - written_total, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(output, bytes.data() + written_total, requested, &written, nullptr) || written == 0U) {
            write_succeeded = false;
            break;
        }
        written_total += written;
    }
    if (write_succeeded) {
        write_succeeded = FlushFileBuffers(output) != FALSE;
    }
    if (!CloseHandle(output)) {
        write_succeeded = false;
    }
    if (write_succeeded) {
        write_succeeded = MoveFileExW(temporary.c_str(), cache_file.c_str(),
                                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!write_succeeded) {
        DeleteFileW(temporary.c_str());
    }
    return write_succeeded;
}

std::wstring PersistentXiaomiCacheKey(const std::filesystem::path& cache_file) {
    std::error_code error;
    auto normalized = std::filesystem::absolute(cache_file, error);
    if (error) {
        normalized = cache_file;
    }
    std::wstring key = normalized.lexically_normal().wstring();
    std::transform(key.begin(), key.end(), key.begin(),
                   [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return key;
}

std::mutex& PersistentXiaomiCacheMutex() {
    static std::mutex mutex;
    return mutex;
}

XiaomiPersistentCacheStores& PersistentXiaomiCacheStore() {
    static XiaomiPersistentCacheStores stores;
    return stores;
}

XiaomiPersistentCacheState* GetPersistentXiaomiCacheState(const std::filesystem::path& cache_file) {
    auto& state = PersistentXiaomiCacheStore()[PersistentXiaomiCacheKey(cache_file)];
    if (!state.loaded) {
        auto loaded = LoadPersistentXiaomiCache(cache_file);
        if (!loaded.has_value()) {
            return nullptr;
        }
        state.entries = std::move(*loaded);
        state.loaded = true;
    }
    return &state;
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

    std::lock_guard<std::mutex> lock(PersistentXiaomiCacheMutex());
    auto* state = GetPersistentXiaomiCacheState(cache_file);
    if (state == nullptr) {
        return;
    }

    XiaomiPersistentCacheEntry entry;
    entry.updated_at_unix = now_unix;
    entry.snapshot = snapshot;
    auto updated_entries = state->entries;
    updated_entries[address] = entry;
    if (SavePersistentXiaomiCache(cache_file, updated_entries)) {
        state->entries = std::move(updated_entries);
    }
}

std::optional<XiaomiBatterySnapshot> GetPersistentXiaomiSnapshot(std::uint64_t address,
                                                                 const std::filesystem::path& cache_file,
                                                                 std::int64_t now_unix,
                                                                 int ttl_minutes) {
    std::lock_guard<std::mutex> lock(PersistentXiaomiCacheMutex());
    auto* state = GetPersistentXiaomiCacheState(cache_file);
    if (state == nullptr) {
        return std::nullopt;
    }

    const auto found = state->entries.find(address);
    if (found == state->entries.end()) {
        return std::nullopt;
    }

    const auto age_seconds = now_unix - found->second.updated_at_unix;
    const auto ttl_seconds = static_cast<std::int64_t>(ttl_minutes) * 60LL;
    if (age_seconds < 0 || age_seconds > ttl_seconds) {
        auto updated_entries = state->entries;
        updated_entries.erase(address);
        if (SavePersistentXiaomiCache(cache_file, updated_entries)) {
            state->entries = std::move(updated_entries);
        }
        return std::nullopt;
    }

    if (!HasAnyBattery(found->second.snapshot)) {
        return std::nullopt;
    }

    return found->second.snapshot;
}

}  // namespace battery_monitor

