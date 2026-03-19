#include "platform/windows/devices/xiaomi/XiaomiModeCache.h"

#include "platform/windows/devices/xiaomi/XiaomiNoiseModeCodec.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace battery_monitor {

namespace {

struct XiaomiModeCacheEntry {
    std::string mode;
    std::string submode;
    std::chrono::steady_clock::time_point captured_at = std::chrono::steady_clock::now();
};

std::unordered_map<std::uint64_t, XiaomiModeCacheEntry>& XiaomiModeCacheStore() {
    static std::unordered_map<std::uint64_t, XiaomiModeCacheEntry> cache;
    return cache;
}

std::mutex& XiaomiModeCacheStoreMutex() {
    static std::mutex mutex;
    return mutex;
}

std::optional<XiaomiModeCacheEntry> TryGetXiaomiModeCacheEntryFull(std::uint64_t address) {
    if (address <= 0xFFFFULL) {
        return std::nullopt;
    }

    const std::lock_guard<std::mutex> lock(XiaomiModeCacheStoreMutex());
    const auto found = XiaomiModeCacheStore().find(address);
    if (found == XiaomiModeCacheStore().end()) {
        return std::nullopt;
    }

    return found->second;
}

}  // namespace

void PutXiaomiModeCacheEntry(std::uint64_t address,
                             std::uint8_t code,
                             std::optional<std::uint8_t> submode_code) {
    if (address <= 0xFFFFULL) {
        return;
    }

    XiaomiModeCacheEntry entry;
    entry.mode = XiaomiNoiseModeCodeToText(code);
    if (submode_code.has_value()) {
        entry.submode = XiaomiNoiseSubmodeCodeToText(code, *submode_code).value_or(std::string());
    }
    entry.captured_at = std::chrono::steady_clock::now();

    const std::lock_guard<std::mutex> lock(XiaomiModeCacheStoreMutex());
    XiaomiModeCacheStore()[address] = std::move(entry);
}

std::optional<std::string> TryGetXiaomiModeCacheEntry(std::uint64_t address) {
    const auto entry = TryGetXiaomiModeCacheEntryFull(address);
    if (!entry.has_value()) {
        return std::nullopt;
    }

    return entry->mode;
}

std::optional<std::string> TryGetXiaomiSubmodeCacheEntry(std::uint64_t address) {
    const auto entry = TryGetXiaomiModeCacheEntryFull(address);
    if (!entry.has_value() || entry->submode.empty()) {
        return std::nullopt;
    }

    return entry->submode;
}

}  // namespace battery_monitor

