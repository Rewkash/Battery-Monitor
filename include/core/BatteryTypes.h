#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace battery_monitor {

struct ProviderOperationContext {
    using Clock = std::chrono::steady_clock;

    std::stop_token stop_token;
    Clock::time_point deadline = Clock::time_point::max();

    bool IsCancelled() const noexcept {
        return stop_token.stop_requested() || Clock::now() >= deadline;
    }

    std::chrono::milliseconds Remaining(std::chrono::milliseconds local_limit) const noexcept {
        if (stop_token.stop_requested()) {
            return std::chrono::milliseconds::zero();
        }
        const auto now = Clock::now();
        if (deadline <= now) {
            return std::chrono::milliseconds::zero();
        }
        if (deadline == Clock::time_point::max()) {
            return local_limit;
        }
        return std::min(local_limit, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
    }
};

struct DeviceBatteryInfo {
    std::string device_id;
    std::string device_name;
    std::string battery_component = "main";
    std::optional<std::uint8_t> battery_level_percent;
    std::optional<std::string> device_mode;
    std::optional<std::string> device_submode;
    std::optional<std::uint16_t> bluetooth_le_appearance;
    std::optional<std::uint32_t> bluetooth_cod_major;
    std::optional<std::uint32_t> bluetooth_cod_minor;
    std::vector<std::string> device_categories;
    bool is_cached = false;
    bool is_connected = true;
};

struct BatteryQueryOptions {
    bool include_disconnected = false;
    bool force_live_refresh = false;
    std::string target_device_id;
    ProviderOperationContext operation;
};

}  // namespace battery_monitor
