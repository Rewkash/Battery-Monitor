#include "platform/windows/shared/WindowsBatteryProviderSupport.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include <winrt/Windows.Foundation.h>

#include "core/DeviceProfiles.h"
#include "platform/windows/devices/phone/BluetoothPnpHints.h"
#include "platform/windows/shared/WindowsBluetoothAddressUtils.h"
#include "platform/windows/devices/controller/WindowsControllerBatteryReader.h"

namespace battery_monitor {

namespace {

using winrt::Windows::Devices::Bluetooth::BluetoothAddressType;
using winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncOperation;

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool ReadBooleanEnvEqualsOne(const char* key, bool fallback);

DeviceProfileQuery MakeWindowsDeviceProfileQuery(const std::string& primary_name,
                                                 const std::string& secondary_name,
                                                 const std::string& device_id) {
    return DeviceProfileQuery{
        .platform = "windows",
        .primary_name = primary_name,
        .secondary_name = secondary_name,
        .device_id = device_id,
    };
}

const LoadedDeviceProfiles& GetWindowsDeviceProfiles() {
    static const LoadedDeviceProfiles& loaded_profiles = GetCachedDeviceProfiles();
    static const bool logged = []() {
        const auto& profiles = GetCachedDeviceProfiles();
        if (!ReadBooleanEnvEqualsOne("BATTERY_MONITOR_DEBUG", false)) {
            return true;
        }

        std::cerr << "[battery-monitor][debug] Device profiles directory: "
                  << profiles.directory.string()
                  << " loaded=" << profiles.profiles.size() << '\n';
        for (const auto& warning : profiles.warnings) {
            std::cerr << "[battery-monitor][debug] " << warning << '\n';
        }
        return true;
    }();
    (void)logged;
    return loaded_profiles;
}

bool HasDeviceProfileFamily(const std::string& primary_name,
                            const std::string& secondary_name,
                            const std::string& device_id,
                            const std::string& family) {
    return AnyMatchingDeviceProfileHasFamily(
        GetWindowsDeviceProfiles(),
        MakeWindowsDeviceProfileQuery(primary_name, secondary_name, device_id),
        family);
}

bool HasDeviceProfileCategory(const std::string& primary_name,
                              const std::string& secondary_name,
                              const std::string& device_id,
                              const std::string& category) {
    return AnyMatchingDeviceProfileHasCategory(
        GetWindowsDeviceProfiles(),
        MakeWindowsDeviceProfileQuery(primary_name, secondary_name, device_id),
        category);
}

bool ReadBooleanEnvEqualsOne(const char* key, bool fallback) {
    char* value = nullptr;
    std::size_t length = 0;
    const errno_t status = _dupenv_s(&value, &length, key);
    if (status == 0 && value != nullptr) {
        const bool enabled = std::string(value) == "1";
        free(value);
        return enabled;
    }
    free(value);
    return fallback;
}

bool ReadBooleanEnvNotZero(const char* key, bool fallback) {
    char* value = nullptr;
    std::size_t length = 0;
    const errno_t status = _dupenv_s(&value, &length, key);
    if (status == 0 && value != nullptr) {
        const bool enabled = std::string(value) != "0";
        free(value);
        return enabled;
    }
    free(value);
    return fallback;
}

int ReadClampedIntEnv(const char* key, int fallback, int min_value, int max_value) {
    char* value = nullptr;
    std::size_t length = 0;
    const errno_t status = _dupenv_s(&value, &length, key);
    if (status == 0 && value != nullptr) {
        try {
            const int parsed = std::stoi(value);
            free(value);
            return std::clamp(parsed, min_value, max_value);
        } catch (const std::exception&) {
        }
    }
    free(value);
    return fallback;
}

std::filesystem::path ResolveXiaomiCacheFilePath() {
    char* value = nullptr;
    std::size_t length = 0;
    const errno_t status = _dupenv_s(&value, &length, "LOCALAPPDATA");

    std::filesystem::path base_path = ".";
    if (status == 0 && value != nullptr && std::string(value).size() > 0U) {
        base_path = value;
    }
    free(value);

    const auto cache_dir = base_path / "BatteryMonitor";
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    return cache_dir / "xiaomi_battery_cache_v1.txt";
}

bool LooksLikeTwsDeviceByName(const std::string& value) {
    if (value.empty()) {
        return false;
    }

    const std::string lowered = ToLowerAscii(value);
    constexpr std::array<const char*, 13> kTwsKeywords = {
        "buds", "earbud", "ear bud", "airpods", "air pods",
        "tws", "in-ear", "inear", "true wireless", "earphone",
        "airdots", "purpods", "pods"};

    for (const auto* keyword : kTwsKeywords) {
        if (lowered.find(keyword) != std::string::npos) {
            return true;
        }
    }

    return false;
}

bool IsLikelyTwsDevice(const std::string& primary_name,
                       const std::string& secondary_name,
                       const std::string& device_id) {
    return LooksLikeTwsDeviceByName(primary_name) ||
           LooksLikeTwsDeviceByName(secondary_name) ||
           LooksLikeTwsDeviceByName(device_id) ||
           HasDeviceProfileCategory(primary_name, secondary_name, device_id, "tws");
}

bool IsLikelyXiaomiEarbuds(const std::string& primary_name,
                           const std::string& secondary_name,
                           const std::string& device_id) {
    const std::string probe = ToLowerAscii(primary_name + " " + secondary_name + " " + device_id);
    const bool has_brand_hint = probe.find("redmi") != std::string::npos ||
                                probe.find("xiaomi") != std::string::npos ||
                                probe.find("mi buds") != std::string::npos ||
                                probe.find("airdots") != std::string::npos ||
                                probe.find("zmi") != std::string::npos ||
                                probe.find("purpods") != std::string::npos;
    const bool has_earbuds_hint = probe.find("bud") != std::string::npos ||
                                  probe.find("airdot") != std::string::npos ||
                                  probe.find("ear") != std::string::npos ||
                                  probe.find("pod") != std::string::npos;
    return (has_brand_hint && has_earbuds_hint) ||
           HasDeviceProfileFamily(primary_name, secondary_name, device_id, "xiaomi_earbuds");
}

bool IsLikelyPhoneDevice(const std::string& primary_name,
                         const std::string& secondary_name,
                         const std::string& device_id) {
    const std::string probe = ToLowerAscii(primary_name + " " + secondary_name + " " + device_id);
    const bool likely_phone_brand = probe.find("poco") != std::string::npos ||
                                    probe.find("xiaomi") != std::string::npos ||
                                    probe.find("redmi") != std::string::npos ||
                                    probe.find("iphone") != std::string::npos ||
                                    probe.find("samsung") != std::string::npos ||
                                    probe.find("huawei") != std::string::npos ||
                                    probe.find("honor") != std::string::npos ||
                                    probe.find("oneplus") != std::string::npos ||
                                    probe.find("pixel") != std::string::npos;
    const bool generic_phone_hint =
        probe.find(" phone") != std::string::npos || probe.find("android") != std::string::npos;
    const bool earbuds_hint = LooksLikeTwsDeviceByName(probe);
    return ((likely_phone_brand || generic_phone_hint) && !earbuds_hint) ||
           HasDeviceProfileCategory(primary_name, secondary_name, device_id, "phone");
}

bool IsLikelyGameControllerDevice(const std::string& primary_name,
                                  const std::string& secondary_name,
                                  const std::string& device_id) {
    const std::string probe = ToLowerAscii(primary_name + " " + secondary_name + " " + device_id);
    return probe.find("wireless controller") != std::string::npos ||
           probe.find("dualshock") != std::string::npos ||
           probe.find("dualsense") != std::string::npos ||
           probe.find("gamepad") != std::string::npos ||
           probe.find("controller") != std::string::npos ||
           probe.find("xbox") != std::string::npos ||
           probe.find("playstation") != std::string::npos ||
           probe.find("ps4") != std::string::npos ||
           probe.find("ps5") != std::string::npos ||
           HasDeviceProfileCategory(primary_name, secondary_name, device_id, "controller");
}

bool ShouldAggressiveXiaomiClassicRetry(const std::string& primary_name,
                                        const std::string& secondary_name,
                                        const std::string& device_id) {
    const std::string probe = ToLowerAscii(primary_name + " " + secondary_name + " " + device_id);
    const bool is_zmi_family = probe.find("zmi") != std::string::npos || probe.find("purpods") != std::string::npos;
    if (is_zmi_family) {
        return false;
    }

    return probe.find("redmi") != std::string::npos ||
           probe.find("xiaomi") != std::string::npos ||
           probe.find("airdots") != std::string::npos;
}

template <typename TResult>
std::optional<TResult> WaitForAsyncResult(IAsyncOperation<TResult> operation, std::chrono::milliseconds timeout) {
    try {
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (operation.Status() == AsyncStatus::Started && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        if (operation.Status() == AsyncStatus::Started) {
            operation.Cancel();
            return std::nullopt;
        }

        if (operation.Status() != AsyncStatus::Completed) {
            return std::nullopt;
        }

        return operation.GetResults();
    } catch (const winrt::hresult_error& error) {
        WindowsBatteryProviderDebugLog(
            "WaitForAsyncResult failed: " + DescribeWinrtBatteryProviderError(error));
        return std::nullopt;
    }
}

std::optional<std::uint8_t> ReadPhoneHfpBatteryHintFromPnpAdapter(std::uint64_t address) {
    return ReadPhoneHfpBatteryHintFromPnpAddress(
        address,
        GetWindowsBatteryProviderRuntimeOptions().debug_enabled,
        &WindowsBatteryProviderDebugLog);
}

std::optional<std::uint8_t> ReadControllerBatteryCachedAdapter(const std::string& device_name,
                                                               const std::string& device_id) {
    return ReadControllerBatteryCached(
        device_name,
        device_id,
        GetWindowsBatteryProviderRuntimeOptions().debug_enabled,
        &WindowsBatteryProviderDebugLog);
}

template <typename TPredicate>
std::optional<ResolvedBluetoothTarget> ResolveBatteryTarget(IBluetoothBatteryProvider* provider,
                                                            const std::string& device_hint,
                                                            bool include_disconnected,
                                                            TPredicate&& predicate) {
    if (provider == nullptr) {
        return std::nullopt;
    }

    BatteryQueryOptions options;
    options.include_disconnected = include_disconnected;
    const auto devices = provider->GetDevicesBattery(options);
    const auto candidates = CollectResolvedBluetoothTargets(devices, device_hint, std::forward<TPredicate>(predicate));
    if (candidates.empty()) {
        return std::nullopt;
    }

    return candidates.front();
}

}  // namespace

const WindowsBatteryProviderRuntimeOptions& GetWindowsBatteryProviderRuntimeOptions() {
    static const WindowsBatteryProviderRuntimeOptions options = []() {
        WindowsBatteryProviderRuntimeOptions value;
        value.debug_enabled = ReadBooleanEnvEqualsOne("BATTERY_MONITOR_DEBUG", false);
        value.generic_scan_enabled = ReadBooleanEnvEqualsOne("BATTERY_MONITOR_GENERIC_SCAN", false);
        value.force_aep_scan = ReadBooleanEnvEqualsOne("BATTERY_MONITOR_FORCE_AEP", false);
        value.persistent_xiaomi_cache_write_enabled = ReadBooleanEnvNotZero("BATTERY_MONITOR_PERSIST_CACHE", true);
        value.persistent_xiaomi_cache_read_enabled = ReadBooleanEnvNotZero("BATTERY_MONITOR_PERSIST_CACHE_READ", true);
        value.xiaomi_cache_ttl_minutes =
            ReadClampedIntEnv("BATTERY_MONITOR_CACHE_TTL_MINUTES", 180, 1, 24 * 60);
        value.xiaomi_cache_file_path = ResolveXiaomiCacheFilePath();
        return value;
    }();
    return options;
}

void WindowsBatteryProviderDebugLog(const std::string& message) {
    if (GetWindowsBatteryProviderRuntimeOptions().debug_enabled) {
        std::cerr << "[battery-monitor][debug] " << message << '\n';
    }
}

std::string DescribeWinrtBatteryProviderError(const winrt::hresult_error& error) {
    std::ostringstream stream;
    stream << "HRESULT=0x" << std::uppercase << std::hex
           << static_cast<std::uint32_t>(error.code().value) << std::dec;
    const auto message = winrt::to_string(error.message());
    if (!message.empty()) {
        stream << " message='" << message << "'";
    }
    return stream.str();
}

void EnsureWindowsBatteryProviderApartmentInitialized() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_changed_state&) {
        // The apartment was already initialized with a different model.
    } catch (const winrt::hresult_error& error) {
        WindowsBatteryProviderDebugLog(
            "init_apartment failed: " + DescribeWinrtBatteryProviderError(error));
    }
}

WindowsBatteryQueryReaderContext MakeWindowsBatteryQueryReaderContext() {
    const auto& runtime_options = GetWindowsBatteryProviderRuntimeOptions();
    return WindowsBatteryQueryReaderContext{
        .debug_enabled = runtime_options.debug_enabled,
        .debug_log = &WindowsBatteryProviderDebugLog,
        .is_likely_tws_device = &IsLikelyTwsDevice,
        .is_likely_phone_device = &IsLikelyPhoneDevice,
        .is_likely_game_controller_device = &IsLikelyGameControllerDevice,
        .looks_like_tws_device_by_name = &LooksLikeTwsDeviceByName,
        .read_phone_hfp_pnp_hint = &ReadPhoneHfpBatteryHintFromPnpAdapter,
        .read_controller_battery = &ReadControllerBatteryCachedAdapter,
    };
}

WindowsBleCandidateBatteryCollectorContext MakeWindowsBleCandidateBatteryCollectorContext() {
    const auto& runtime_options = GetWindowsBatteryProviderRuntimeOptions();
    return WindowsBleCandidateBatteryCollectorContext{
        .debug_enabled = runtime_options.debug_enabled,
        .debug_log = &WindowsBatteryProviderDebugLog,
        .is_likely_tws_device = &IsLikelyTwsDevice,
        .is_likely_xiaomi_earbuds = &IsLikelyXiaomiEarbuds,
        .should_aggressive_xiaomi_classic_retry = &ShouldAggressiveXiaomiClassicRetry,
    };
}

WindowsTwsCandidateBatteryCollectorContext MakeWindowsTwsCandidateBatteryCollectorContext(
    bool include_disconnected) {
    const auto& runtime_options = GetWindowsBatteryProviderRuntimeOptions();
    return WindowsTwsCandidateBatteryCollectorContext{
        .debug_enabled = runtime_options.debug_enabled,
        .debug_log = &WindowsBatteryProviderDebugLog,
        .include_disconnected = include_disconnected,
        .force_aep_scan = runtime_options.force_aep_scan,
        .is_likely_xiaomi_earbuds = &IsLikelyXiaomiEarbuds,
        .should_aggressive_xiaomi_classic_retry = &ShouldAggressiveXiaomiClassicRetry,
        .open_ble_device_by_address = &TryOpenBleDeviceByAddress,
    };
}

XiaomiControlActionContext MakeWindowsXiaomiControlActionContext() {
    const auto& runtime_options = GetWindowsBatteryProviderRuntimeOptions();
    return XiaomiControlActionContext{
        .debug_enabled = runtime_options.debug_enabled,
        .debug_log = &WindowsBatteryProviderDebugLog,
    };
}

std::optional<BluetoothLEDevice> TryOpenBleDeviceByAddress(std::uint64_t address,
                                                           std::chrono::milliseconds timeout) {
    auto wait_for_operation = [timeout](auto operation) -> std::optional<BluetoothLEDevice> {
        try {
            const auto deadline = std::chrono::steady_clock::now() + timeout;

            while (operation.Status() == AsyncStatus::Started &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            if (operation.Status() == AsyncStatus::Started) {
                operation.Cancel();
                return std::nullopt;
            }

            if (operation.Status() != AsyncStatus::Completed) {
                return std::nullopt;
            }

            auto device = operation.GetResults();
            if (!device) {
                return std::nullopt;
            }

            return device;
        } catch (const winrt::hresult_error& error) {
            WindowsBatteryProviderDebugLog(
                "TryOpenBleDeviceByAddress operation failed: " +
                DescribeWinrtBatteryProviderError(error));
            return std::nullopt;
        }
    };

    if (const auto device = wait_for_operation(BluetoothLEDevice::FromBluetoothAddressAsync(address));
        device.has_value()) {
        return device;
    }

    if (const auto device =
            wait_for_operation(BluetoothLEDevice::FromBluetoothAddressAsync(address, BluetoothAddressType::Public));
        device.has_value()) {
        return device;
    }

    if (const auto device =
            wait_for_operation(BluetoothLEDevice::FromBluetoothAddressAsync(address, BluetoothAddressType::Random));
        device.has_value()) {
        return device;
    }

    return std::nullopt;
}

std::optional<ResolvedBluetoothTarget> ResolveConnectedXiaomiControlTarget(
    IBluetoothBatteryProvider* provider,
    const std::string& device_hint) {
    return ResolveBatteryTarget(
        provider,
        device_hint,
        false,
        [](const DeviceBatteryInfo& entry, const std::string& normalized_hint) {
            if (!entry.is_connected) {
                return false;
            }
            if (!IsLikelyXiaomiEarbuds(entry.device_name, entry.device_name, entry.device_id)) {
                return false;
            }
            return DeviceBatteryEntryMatchesHint(entry, normalized_hint);
        });
}

}  // namespace battery_monitor

