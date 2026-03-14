#include "platform/windows/WinRtBatteryProvider.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <iostream>

#include <winsock2.h>
#include <ws2bth.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Power.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#pragma comment(lib, "Cfgmgr32.lib")
#pragma comment(lib, "Setupapi.lib")
#pragma comment(lib, "Hid.lib")

namespace battery_monitor {

namespace {

using winrt::Windows::Devices::Bluetooth::BluetoothCacheMode;
using winrt::Windows::Devices::Bluetooth::BluetoothAddressType;
using winrt::Windows::Devices::Bluetooth::BluetoothDevice;
using winrt::Windows::Devices::Bluetooth::BluetoothConnectionStatus;
using winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
using winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs;
using winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher;
using winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEScanningMode;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristicProperties;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristicUuids;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattServiceUuids;
using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Devices::Enumeration::DeviceInformationKind;
using winrt::Windows::Gaming::Input::IGameControllerBatteryInfo;
using winrt::Windows::Gaming::Input::Gamepad;
using winrt::Windows::Gaming::Input::RawGameController;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncOperation;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::Foundation::IPropertyValue;
using winrt::Windows::Foundation::PropertyType;
using winrt::Windows::Storage::Streams::DataReader;

constexpr const wchar_t* kDeviceContainerCategoryProperty =
    L"{78C34FC8-104A-4ACA-9EA4-524D52996E57} 90";
constexpr const wchar_t* kDeviceContainerPrimaryCategoryProperty =
    L"{78C34FC8-104A-4ACA-9EA4-524D52996E57} 97";

struct BatteryReading {
    std::string component;
    std::uint8_t percent = 0;
};

struct XiaomiBatterySnapshot {
    std::optional<std::uint8_t> left;
    std::optional<std::uint8_t> right;
    std::optional<std::uint8_t> case_level;
};

struct XiaomiReadResult {
    std::vector<BatteryReading> readings;
    bool from_persistent_cache = false;
};

struct EndpointCandidate {
    std::string endpoint_id;
    std::string endpoint_name;
    std::uint64_t bluetooth_address = 0;
    std::optional<std::uint16_t> bluetooth_le_appearance;
    std::optional<std::uint32_t> bluetooth_cod_major;
    std::optional<std::uint32_t> bluetooth_cod_minor;
    std::vector<std::string> device_categories;
    bool from_connected_scan = false;
    bool is_connected = false;
};

struct PnpBluetoothVisualHints {
    std::optional<std::uint32_t> bluetooth_cod_major;
    std::optional<std::uint32_t> bluetooth_cod_minor;
    std::vector<std::string> device_categories;
};

enum class XiaomiMessageType : std::uint8_t {
    kPhoneRequest = 0xC4,
    kResponse = 0x04,
    kEarbudsRequest = 0xC0,
    kEarbudsNotify = 0xC7
};

enum class XiaomiOpcode : std::uint8_t {
    kGetDeviceInfo = 0x02,
    kGetDeviceRunInfo = 0x09,
    kReportStatus = 0x0E,
    kAuthChallenge = 0x50,
    kAuthConfirm = 0x51
};

struct XiaomiMessage {
    XiaomiMessageType type = XiaomiMessageType::kPhoneRequest;
    XiaomiOpcode opcode = XiaomiOpcode::kGetDeviceInfo;
    std::uint8_t sequence = 0;
    std::vector<std::uint8_t> payload;
};

constexpr GUID kXiaomiDeviceCtrlServiceUuid = {0x0000FD2D, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};
constexpr GUID kBluetoothSerialPortServiceUuid = {0x00001101, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};
constexpr GUID kZmiPurPodsSerialServiceUuid = {0x00001101, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x85, 0x84, 0xD0, 0x18, 0x10}};
constexpr GUID kHandsfreeAudioGatewayServiceUuid = {0x0000111E, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};
constexpr DEVPROPKEY kPhoneHfpBatteryHintPropKey = {
    {0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}},
    2};
constexpr DEVPROPKEY kZmiVendorBatteryHintPropKey = {
    {0x670245F9, 0x6E25, 0x4179, {0x85, 0xC1, 0x98, 0x1C, 0x33, 0xB9, 0xD3, 0xB7}},
    4};
constexpr DEVPROPKEY kBluetoothClassOfDevicePropKey = {
    {0x2BD67D8B, 0x8BEB, 0x48D5, {0x87, 0xE0, 0x6C, 0xDA, 0x34, 0x28, 0x04, 0x0A}},
    4};
constexpr DEVPROPKEY kDeviceContainerCategoryPropKey = {
    {0x78C34FC8, 0x104A, 0x4ACA, {0x9E, 0xA4, 0x52, 0x4D, 0x52, 0x99, 0x6E, 0x57}},
    90};
constexpr DEVPROPKEY kDeviceContainerPrimaryCategoryPropKey = {
    {0x78C34FC8, 0x104A, 0x4ACA, {0x9E, 0xA4, 0x52, 0x4D, 0x52, 0x99, 0x6E, 0x57}},
    97};

constexpr std::array<std::uint8_t, 3> kXiaomiMessageHeader = {0xFE, 0xDC, 0xBA};
constexpr std::uint8_t kXiaomiMessageTrailer = 0xEF;
constexpr int kXiaomiAuthPattern = 0x9999;

constexpr std::array<std::uint8_t, 16> kXiaomiAuthSeq = {
    0x11, 0x22, 0x33, 0x33, 0x22, 0x11, 0x11, 0x22,
    0x33, 0x33, 0x22, 0x11, 0x11, 0x22, 0x33, 0x33};

constexpr std::array<std::array<int, 16>, 16> kXiaomiAuthCoefficients = {{
    {{2,1,1,1,4,2,1,1,2,2,4,2,4,4,16,8}},
    {{2,1,1,1,4,2,1,1,1,1,2,1,2,2,8,4}},
    {{1,1,4,2,2,2,4,2,16,8,4,4,2,1,1,1}},
    {{1,1,4,2,1,1,2,1,8,4,2,2,2,1,1,1}},
    {{16,8,2,2,4,2,4,4,1,1,4,2,1,1,2,1}},
    {{8,4,1,1,2,1,2,2,1,1,4,2,1,1,2,1}},
    {{2,2,4,2,4,4,16,8,2,1,1,1,4,2,1,1}},
    {{1,1,2,1,2,2,8,4,2,1,1,1,4,2,1,1}},
    {{4,2,4,4,16,8,2,2,1,1,2,1,1,1,4,2}},
    {{2,1,2,2,8,4,1,1,1,1,2,1,1,1,4,2}},
    {{4,4,16,8,1,1,2,1,4,2,1,1,4,2,2,2}},
    {{2,2,8,4,1,1,2,1,4,2,1,1,2,1,1,1}},
    {{1,1,2,1,1,1,4,2,4,4,16,8,2,2,4,2}},
    {{1,1,2,1,1,1,4,2,2,2,8,4,1,1,2,1}},
    {{4,2,1,1,2,1,1,1,4,2,2,2,16,8,4,4}},
    {{4,2,1,1,2,1,1,1,2,1,1,1,8,4,2,2}}
}};

class ScopedWsa {
   public:
    ScopedWsa() {
        started_ = (WSAStartup(MAKEWORD(2, 2), &wsa_data_) == 0);
    }

    ~ScopedWsa() {
        if (started_) {
            WSACleanup();
        }
    }

    bool started() const {
        return started_;
    }

   private:
    WSADATA wsa_data_{};
    bool started_ = false;
};

bool DebugEnabled() {
    static const bool enabled = []() {
        char* value = nullptr;
        std::size_t length = 0;
        const errno_t status = _dupenv_s(&value, &length, "BATTERY_MONITOR_DEBUG");
        const bool enabled_flag = (status == 0 && value != nullptr && std::string(value) == "1");
        free(value);
        return enabled_flag;
    }();
    return enabled;
}

bool GenericScanEnabled() {
    static const bool enabled = []() {
        char* value = nullptr;
        std::size_t length = 0;
        const errno_t status = _dupenv_s(&value, &length, "BATTERY_MONITOR_GENERIC_SCAN");
        const bool enabled_flag = (status == 0 && value != nullptr && std::string(value) == "1");
        free(value);
        return enabled_flag;
    }();
    return enabled;
}

bool ForceAepScanEnabled() {
    static const bool enabled = []() {
        char* value = nullptr;
        std::size_t length = 0;
        const errno_t status = _dupenv_s(&value, &length, "BATTERY_MONITOR_FORCE_AEP");
        const bool enabled_flag = (status == 0 && value != nullptr && std::string(value) == "1");
        free(value);
        return enabled_flag;
    }();
    return enabled;
}

bool PersistentXiaomiCacheWriteEnabled() {
    static const bool enabled = []() {
        char* value = nullptr;
        std::size_t length = 0;
        const errno_t status = _dupenv_s(&value, &length, "BATTERY_MONITOR_PERSIST_CACHE");
        if (status == 0 && value != nullptr) {
            const std::string text(value);
            free(value);
            return text != "0";
        }
        free(value);
        return true;
    }();
    return enabled;
}

bool PersistentXiaomiCacheReadEnabled() {
    static const bool enabled = []() {
        char* value = nullptr;
        std::size_t length = 0;
        const errno_t status = _dupenv_s(&value, &length, "BATTERY_MONITOR_PERSIST_CACHE_READ");
        if (status == 0 && value != nullptr) {
            const std::string text(value);
            free(value);
            return text != "0";
        }
        free(value);
        return true;
    }();
    return enabled;
}

int XiaomiCacheTtlMinutes() {
    static const int ttl_minutes = []() {
        char* value = nullptr;
        std::size_t length = 0;
        const errno_t status = _dupenv_s(&value, &length, "BATTERY_MONITOR_CACHE_TTL_MINUTES");
        if (status == 0 && value != nullptr) {
            try {
                const int parsed = std::stoi(value);
                free(value);
                return parsed > 0 ? parsed : 180;
            } catch (const std::exception&) {
            }
        }
        free(value);
        return 180;
    }();
    return ttl_minutes;
}

int XiaomiAdvertisementScanMs() {
    static const int scan_ms = []() {
        char* value = nullptr;
        std::size_t length = 0;
        const errno_t status = _dupenv_s(&value, &length, "BATTERY_MONITOR_XIAOMI_ADV_SCAN_MS");
        if (status == 0 && value != nullptr) {
            try {
                const int parsed = std::stoi(value);
                free(value);
                return std::clamp(parsed, 300, 6000);
            } catch (const std::exception&) {
            }
        }
        free(value);
        return 1800;
    }();
    return scan_ms;
}

int ZmiObserveMs() {
    static const int observe_ms = []() {
        char* value = nullptr;
        std::size_t length = 0;
        const errno_t status = _dupenv_s(&value, &length, "BATTERY_MONITOR_ZMI_OBSERVE_MS");
        if (status == 0 && value != nullptr) {
            try {
                const int parsed = std::stoi(value);
                free(value);
                return std::clamp(parsed, 0, 30000);
            } catch (const std::exception&) {
            }
        }
        free(value);
        return 0;
    }();
    return observe_ms;
}

std::filesystem::path XiaomiCacheFilePath() {
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

std::int64_t CurrentUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void DebugLog(const std::string& message) {
    if (DebugEnabled()) {
        std::cerr << "[battery-monitor][debug] " << message << '\n';
    }
}

std::string BytesToHex(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return {};
    }

    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(bytes.size() * 3U);

    for (const auto value : bytes) {
        output.push_back(kHex[(value >> 4U) & 0x0FU]);
        output.push_back(kHex[value & 0x0FU]);
        output.push_back(' ');
    }

    return output;
}

std::string ByteToHex(std::uint8_t value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string output = "0x";
    output.push_back(kHex[(value >> 4U) & 0x0FU]);
    output.push_back(kHex[value & 0x0FU]);
    return output;
}

std::string DescribeHresultError(const winrt::hresult_error& error) {
    std::ostringstream stream;
    stream << "HRESULT=0x" << std::uppercase << std::hex
           << static_cast<std::uint32_t>(error.code().value) << std::dec;
    const auto message = winrt::to_string(error.message());
    if (!message.empty()) {
        stream << " message='" << message << "'";
    }
    return stream.str();
}

void EnsureApartmentInitialized() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_changed_state&) {
        // The apartment was already initialized with a different model.
    } catch (const winrt::hresult_error& error) {
        DebugLog("init_apartment failed: " + DescribeHresultError(error));
    }
}

std::string ToUtf8(const winrt::hstring& value) {
    return winrt::to_string(value);
}

std::string ToUtf8(const std::wstring& value) {
    return winrt::to_string(winrt::hstring(value));
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string NormalizeComponentHint(const std::string& hint) {
    if (hint.empty()) {
        return {};
    }

    const std::string lowered = ToLowerAscii(hint);
    if (lowered.find("left") != std::string::npos || lowered == "l" || lowered.find(" l ") != std::string::npos) {
        return "left";
    }
    if (lowered.find("right") != std::string::npos || lowered == "r" || lowered.find(" r ") != std::string::npos) {
        return "right";
    }
    if (lowered.find("case") != std::string::npos || lowered.find("box") != std::string::npos) {
        return "case";
    }
    if (lowered.find("main") != std::string::npos) {
        return "main";
    }

    return {};
}

bool LooksLikeTwsDeviceByName(const std::string& value) {
    if (value.empty()) {
        return false;
    }

    const std::string lowered = ToLowerAscii(value);
    constexpr std::array<const char*, 13> kTwsKeywords = {
        "buds",   "earbud", "ear bud", "airpods", "air pods",
        "tws",    "in-ear", "inear",   "true wireless", "earphone",
        "airdots", "purpods", "pods"};

    for (const auto* keyword : kTwsKeywords) {
        if (lowered.find(keyword) != std::string::npos) {
            return true;
        }
    }

    return false;
}

bool IsLikelyTwsDevice(const std::string& primary_name, const std::string& secondary_name, const std::string& device_id) {
    return LooksLikeTwsDeviceByName(primary_name) || LooksLikeTwsDeviceByName(secondary_name) ||
           LooksLikeTwsDeviceByName(device_id);
}

bool IsLikelyXiaomiEarbuds(const std::string& primary_name, const std::string& secondary_name,
                           const std::string& device_id) {
    const std::string probe = ToLowerAscii(primary_name + " " + secondary_name + " " + device_id);
    const bool has_brand_hint = probe.find("redmi") != std::string::npos || probe.find("xiaomi") != std::string::npos ||
                                probe.find("mi buds") != std::string::npos || probe.find("airdots") != std::string::npos ||
                                probe.find("zmi") != std::string::npos ||
                                probe.find("purpods") != std::string::npos;
    const bool has_earbuds_hint = probe.find("bud") != std::string::npos || probe.find("airdot") != std::string::npos ||
                                  probe.find("ear") != std::string::npos || probe.find("pod") != std::string::npos;
    return has_brand_hint && has_earbuds_hint;
}

bool IsLikelyZmiPurPods(const std::string& primary_name, const std::string& secondary_name,
                        const std::string& device_id) {
    const std::string probe = ToLowerAscii(primary_name + " " + secondary_name + " " + device_id);
    return probe.find("zmi") != std::string::npos || probe.find("purpods") != std::string::npos;
}

bool IsLikelyPhoneDevice(const std::string& primary_name, const std::string& secondary_name,
                         const std::string& device_id) {
    const std::string probe = ToLowerAscii(primary_name + " " + secondary_name + " " + device_id);
    const bool likely_phone_brand =
        probe.find("poco") != std::string::npos ||
        probe.find("xiaomi") != std::string::npos ||
        probe.find("redmi") != std::string::npos ||
        probe.find("iphone") != std::string::npos ||
        probe.find("samsung") != std::string::npos ||
        probe.find("huawei") != std::string::npos ||
        probe.find("honor") != std::string::npos ||
        probe.find("oneplus") != std::string::npos ||
        probe.find("pixel") != std::string::npos;
    const bool generic_phone_hint =
        probe.find(" phone") != std::string::npos ||
        probe.find("android") != std::string::npos;
    const bool earbuds_hint = LooksLikeTwsDeviceByName(probe);
    return (likely_phone_brand || generic_phone_hint) && !earbuds_hint;
}

bool IsLikelyGameControllerDevice(const std::string& primary_name, const std::string& secondary_name,
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
           probe.find("ps5") != std::string::npos;
}

bool IsLikelySonyDualShockController(const std::string& primary_name, const std::string& secondary_name,
                                     const std::string& device_id) {
    const std::string probe = ToLowerAscii(primary_name + " " + secondary_name + " " + device_id);
    return probe.find("wireless controller") != std::string::npos ||
           probe.find("dualshock") != std::string::npos ||
           probe.find("dualsense") != std::string::npos ||
           probe.find("vid&0002054c") != std::string::npos;
}

std::string NormalizeControllerNameForMatch(const std::string& value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const char ch : ToLowerAscii(value)) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            normalized.push_back(ch);
        }
    }
    return normalized;
}

std::optional<std::uint8_t> TryConvertBatteryReportToPercent(
    const winrt::Windows::Devices::Power::BatteryReport& report) {
    if (!report) {
        return std::nullopt;
    }

    const auto full_capacity = report.FullChargeCapacityInMilliwattHours();
    const auto remaining_capacity = report.RemainingCapacityInMilliwattHours();
    if (full_capacity && remaining_capacity) {
        const auto full_value = full_capacity.Value();
        const auto remaining_value = remaining_capacity.Value();
        if (full_value > 0) {
            const auto percent = static_cast<int>((static_cast<std::int64_t>(remaining_value) * 100LL) / full_value);
            return static_cast<std::uint8_t>(std::clamp(percent, 0, 100));
        }
    }

    return std::nullopt;
}

std::optional<std::uint64_t> ParseBluetoothAddressFromDeviceId(const std::string& device_id);

struct DualShockHidRouteCacheEntry {
    std::wstring path;
    std::wstring instance_id;
};

struct ControllerBatteryCacheEntry {
    std::optional<std::uint8_t> value;
    std::chrono::steady_clock::time_point captured_at = std::chrono::steady_clock::now();
};

struct XiaomiModeCacheEntry {
    std::string mode;
    std::string submode;
    std::chrono::steady_clock::time_point captured_at = std::chrono::steady_clock::now();
};

std::unordered_map<std::string, DualShockHidRouteCacheEntry>& DualShockHidRouteCache() {
    static std::unordered_map<std::string, DualShockHidRouteCacheEntry> cache;
    return cache;
}

std::mutex& DualShockHidRouteCacheMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, ControllerBatteryCacheEntry>& ControllerBatteryCacheStore() {
    static std::unordered_map<std::string, ControllerBatteryCacheEntry> cache;
    return cache;
}

std::mutex& ControllerBatteryCacheStoreMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::uint64_t, XiaomiModeCacheEntry>& XiaomiModeCacheStore() {
    static std::unordered_map<std::uint64_t, XiaomiModeCacheEntry> cache;
    return cache;
}

std::mutex& XiaomiModeCacheStoreMutex() {
    static std::mutex mutex;
    return mutex;
}

int ControllerBatteryCacheTtlMs() {
    static const int ttl_ms = []() {
        char* value = nullptr;
        std::size_t length = 0;
        const errno_t status = _dupenv_s(&value, &length, "BATTERY_MONITOR_CONTROLLER_CACHE_MS");
        int parsed = 45000;
        if (status == 0 && value != nullptr) {
            try {
                parsed = std::stoi(value);
            } catch (...) {
                parsed = 45000;
            }
        }
        free(value);
        return std::clamp(parsed, 0, 300000);
    }();
    return ttl_ms;
}

std::optional<ControllerBatteryCacheEntry> TryGetControllerBatteryCacheEntry(const std::string& cache_key) {
    if (cache_key.empty()) {
        return std::nullopt;
    }
    const std::lock_guard<std::mutex> lock(ControllerBatteryCacheStoreMutex());
    const auto found = ControllerBatteryCacheStore().find(cache_key);
    if (found == ControllerBatteryCacheStore().end()) {
        return std::nullopt;
    }
    return found->second;
}

void PutControllerBatteryCacheEntry(const std::string& cache_key, std::optional<std::uint8_t> value) {
    if (cache_key.empty()) {
        return;
    }
    ControllerBatteryCacheEntry entry;
    entry.value = value;
    entry.captured_at = std::chrono::steady_clock::now();
    const std::lock_guard<std::mutex> lock(ControllerBatteryCacheStoreMutex());
    ControllerBatteryCacheStore()[cache_key] = entry;
}

std::string BuildDualShockHidRouteCacheKey(const std::string& device_name_hint,
                                           const std::string& device_id_hint) {
    if (const auto address = ParseBluetoothAddressFromDeviceId(device_id_hint); address.has_value()) {
        std::ostringstream stream;
        stream << "addr:" << std::uppercase << std::hex << std::setw(12) << std::setfill('0') << *address;
        return stream.str();
    }
    const std::string normalized_id = NormalizeControllerNameForMatch(device_id_hint);
    if (!normalized_id.empty()) {
        return "id:" + normalized_id;
    }
    const std::string normalized_name = NormalizeControllerNameForMatch(device_name_hint);
    if (!normalized_name.empty()) {
        return "name:" + normalized_name;
    }
    return {};
}

std::optional<DualShockHidRouteCacheEntry> TryGetDualShockHidRoute(const std::string& cache_key) {
    if (cache_key.empty()) {
        return std::nullopt;
    }
    const std::lock_guard<std::mutex> lock(DualShockHidRouteCacheMutex());
    const auto found = DualShockHidRouteCache().find(cache_key);
    if (found == DualShockHidRouteCache().end()) {
        return std::nullopt;
    }
    return found->second;
}

void PutDualShockHidRoute(const std::string& cache_key,
                          const std::wstring& path,
                          const std::wstring& instance_id) {
    if (cache_key.empty() || path.empty()) {
        return;
    }
    const std::lock_guard<std::mutex> lock(DualShockHidRouteCacheMutex());
    DualShockHidRouteCache()[cache_key] = DualShockHidRouteCacheEntry{path, instance_id};
}

void EraseDualShockHidRoute(const std::string& cache_key) {
    if (cache_key.empty()) {
        return;
    }
    const std::lock_guard<std::mutex> lock(DualShockHidRouteCacheMutex());
    DualShockHidRouteCache().erase(cache_key);
}

std::optional<std::uint8_t> ParseDualShockBatteryPercentFromStatusByte(std::uint8_t status_byte) {
    const bool cable_connected = (status_byte & 0x10U) != 0U;
    const std::uint8_t battery_data = static_cast<std::uint8_t>(status_byte & 0x0FU);

    if (battery_data < 10U) {
        // DS4 reports coarse buckets: 0=0..9%, 1=10..19%, ... 9=90..99%.
        // Kernel maps those to midpoint values.
        return static_cast<std::uint8_t>(battery_data * 10U + 5U);
    }
    if (battery_data == 10U) {
        return static_cast<std::uint8_t>(100U);
    }
    if (cable_connected && battery_data == 11U) {
        return static_cast<std::uint8_t>(100U);
    }
    // 14/15 while charging mean charge error/temperature-voltage issue.
    return std::nullopt;
}

std::optional<std::uint8_t> ParseDualShockBatteryFromInputReport(const std::vector<std::uint8_t>& report,
                                                                  std::size_t* used_offset) {
    if (report.empty()) {
        return std::nullopt;
    }

    const std::uint8_t report_id = report[0];
    if (report_id != 0x01U && report_id != 0x11U) {
        return std::nullopt;
    }

    std::array<std::size_t, 3> offsets = {30U, 12U, 32U};
    if (report_id == 0x01U) {
        offsets = {12U, 30U, 32U};
    } else if (report_id == 0x11U) {
        // BT report has 2-byte header before common payload, so status[0] is at 32.
        offsets = {32U, 30U, 12U};
    }

    for (const auto offset : offsets) {
        if (offset >= report.size()) {
            continue;
        }
        const auto parsed = ParseDualShockBatteryPercentFromStatusByte(report[offset]);
        if (!parsed.has_value()) {
            continue;
        }
        if (used_offset != nullptr) {
            *used_offset = offset;
        }
        return parsed;
    }

    return std::nullopt;
}

std::optional<std::uint8_t> TryReadDualShockBatteryFromHid(const std::string& device_name_hint,
                                                            const std::string& device_id_hint) {
    GUID hid_guid{};
    HidD_GetHidGuid(&hid_guid);

    const HDEVINFO device_info_set = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr,
                                                          DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (device_info_set == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    struct DeviceInfoSetGuard {
        HDEVINFO handle = INVALID_HANDLE_VALUE;
        ~DeviceInfoSetGuard() {
            if (handle != INVALID_HANDLE_VALUE) {
                SetupDiDestroyDeviceInfoList(handle);
            }
        }
    } device_info_guard{device_info_set};

    std::optional<std::uint64_t> address_hint = ParseBluetoothAddressFromDeviceId(device_id_hint);
    std::string address_hint_token;
    if (address_hint.has_value()) {
        std::ostringstream stream;
        stream << std::uppercase << std::hex << std::setw(12) << std::setfill('0') << *address_hint;
        address_hint_token = stream.str();
    }
    const std::string cache_key = BuildDualShockHidRouteCacheKey(device_name_hint, device_id_hint);

    auto try_read_candidate = [&](const std::wstring& path,
                                  const std::wstring& instance_id) -> std::optional<std::uint8_t> {
        HANDLE device_handle = CreateFileW(path.c_str(),
                                           GENERIC_READ | GENERIC_WRITE,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           nullptr,
                                           OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                           nullptr);
        if (device_handle == INVALID_HANDLE_VALUE) {
            device_handle = CreateFileW(path.c_str(),
                                        GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                        nullptr);
        }
        if (device_handle == INVALID_HANDLE_VALUE) {
            device_handle = CreateFileW(path.c_str(),
                                        GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
        }
        if (device_handle == INVALID_HANDLE_VALUE) {
            return std::nullopt;
        }

        struct HandleGuard {
            HANDLE handle = INVALID_HANDLE_VALUE;
            ~HandleGuard() {
                if (handle != INVALID_HANDLE_VALUE) {
                    CloseHandle(handle);
                }
            }
        } handle_guard{device_handle};

        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        if (!HidD_GetAttributes(device_handle, &attributes)) {
            return std::nullopt;
        }
        if (attributes.VendorID != 0x054CU) {
            return std::nullopt;
        }

        PHIDP_PREPARSED_DATA preparsed_data = nullptr;
        USHORT input_report_len = 78U;
        if (HidD_GetPreparsedData(device_handle, &preparsed_data)) {
            HIDP_CAPS caps{};
            if (HidP_GetCaps(preparsed_data, &caps) == HIDP_STATUS_SUCCESS && caps.InputReportByteLength > 0U) {
                input_report_len = caps.InputReportByteLength;
            }
            HidD_FreePreparsedData(preparsed_data);
        }
        if (input_report_len < 32U) {
            input_report_len = 78U;
        }

        std::vector<std::uint8_t> report(static_cast<std::size_t>(input_report_len), 0U);
        bool got_report = false;
        for (const std::uint8_t report_id : {0x11U, 0x01U}) {
            std::fill(report.begin(), report.end(), 0U);
            report[0] = report_id;
            if (HidD_GetInputReport(device_handle, report.data(), static_cast<ULONG>(report.size()))) {
                got_report = true;
                break;
            }
        }
        if (!got_report) {
            HidD_FlushQueue(device_handle);
            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (overlapped.hEvent != nullptr) {
                DWORD bytes_read = 0;
                std::fill(report.begin(), report.end(), 0U);
                BOOL read_result = ReadFile(device_handle, report.data(), static_cast<DWORD>(report.size()),
                                            nullptr, &overlapped);
                if (!read_result) {
                    const DWORD read_error = GetLastError();
                    if (read_error == ERROR_IO_PENDING) {
                        const DWORD wait_result = WaitForSingleObject(overlapped.hEvent, 260);
                        if (wait_result == WAIT_OBJECT_0) {
                            read_result = GetOverlappedResult(device_handle, &overlapped, &bytes_read, FALSE);
                        } else {
                            CancelIo(device_handle);
                        }
                    }
                } else {
                    read_result = GetOverlappedResult(device_handle, &overlapped, &bytes_read, FALSE);
                }

                if (read_result && bytes_read > 0U) {
                    report.resize(bytes_read);
                    got_report = true;
                }
                CloseHandle(overlapped.hEvent);
            }
        }
        if (!got_report) {
            if (DebugEnabled()) {
                DebugLog("DualShock HID fallback: no input report for instance='" +
                         ToUtf8(winrt::hstring(instance_id)) + "'");
            }
            return std::nullopt;
        }

        std::size_t used_offset = 0;
        const auto parsed = ParseDualShockBatteryFromInputReport(report, &used_offset);
        if (!parsed.has_value()) {
            if (DebugEnabled()) {
                DebugLog("DualShock HID fallback: report parsed but battery offset was not recognized for instance='" +
                         ToUtf8(winrt::hstring(instance_id)) + "'");
            }
            return std::nullopt;
        }

        if (DebugEnabled()) {
            DebugLog("DualShock HID battery fallback accepted value=" + std::to_string(*parsed) +
                     " reportId=0x" + BytesToHex(std::vector<std::uint8_t>{report[0]}) +
                     " offset=" + std::to_string(used_offset) +
                     " instance='" + ToUtf8(winrt::hstring(instance_id)) + "'");
        }
        return parsed;
    };

    if (const auto cached_route = TryGetDualShockHidRoute(cache_key); cached_route.has_value()) {
        if (DebugEnabled()) {
            DebugLog("DualShock HID fallback: trying cached route for key='" + cache_key + "'");
        }
        if (const auto cached_reading = try_read_candidate(cached_route->path, cached_route->instance_id);
            cached_reading.has_value()) {
            return cached_reading;
        }
        if (DebugEnabled()) {
            DebugLog("DualShock HID fallback: cached route failed, rescanning candidates for key='" + cache_key + "'");
        }
        EraseDualShockHidRoute(cache_key);
    }

    struct HidCandidate {
        std::wstring path;
        std::wstring instance_id;
        int score = 0;
    };
    std::vector<HidCandidate> candidates;

    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data{};
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(device_info_set, nullptr, &hid_guid, index, &interface_data)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            continue;
        }

        DWORD required_size = 0;
        SetupDiGetDeviceInterfaceDetailW(device_info_set, &interface_data, nullptr, 0, &required_size, nullptr);
        if (required_size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            continue;
        }

        std::vector<std::uint8_t> detail_buffer(required_size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail_buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        SP_DEVINFO_DATA devinfo_data{};
        devinfo_data.cbSize = sizeof(devinfo_data);
        if (!SetupDiGetDeviceInterfaceDetailW(device_info_set, &interface_data, detail, required_size, nullptr,
                                              &devinfo_data)) {
            continue;
        }

        wchar_t instance_id_buffer[512] = {};
        std::wstring instance_id;
        if (SetupDiGetDeviceInstanceIdW(device_info_set, &devinfo_data, instance_id_buffer,
                                        static_cast<DWORD>(std::size(instance_id_buffer)), nullptr)) {
            instance_id.assign(instance_id_buffer);
        }

        std::wstring normalized_instance = instance_id;
        std::transform(normalized_instance.begin(), normalized_instance.end(), normalized_instance.begin(),
                       [](wchar_t value) { return static_cast<wchar_t>(std::towupper(value)); });

        int score = 0;
        if (normalized_instance.find(L"VID&0002054C") != std::wstring::npos) {
            score += 60;
        }
        if (normalized_instance.find(L"PID&05C4") != std::wstring::npos ||
            normalized_instance.find(L"PID&09CC") != std::wstring::npos ||
            normalized_instance.find(L"PID&0CE6") != std::wstring::npos) {
            score += 40;
        }
        if (normalized_instance.find(L"{00001124-0000-1000-8000-00805F9B34FB}") != std::wstring::npos) {
            score += 20;
        }
        if (!address_hint_token.empty()) {
            const auto token = winrt::to_hstring(address_hint_token);
            if (normalized_instance.find(token.c_str()) != std::wstring::npos) {
                score += 35;
            }
        }
        if (IsLikelySonyDualShockController(device_name_hint, device_name_hint, device_id_hint)) {
            score += 10;
        }
        if (score <= 0) {
            continue;
        }

        HidCandidate candidate;
        candidate.path = detail->DevicePath;
        candidate.instance_id = std::move(instance_id);
        candidate.score = score;
        candidates.push_back(std::move(candidate));
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const HidCandidate& lhs, const HidCandidate& rhs) {
                  return lhs.score > rhs.score;
              });
    if (DebugEnabled()) {
        DebugLog("DualShock HID fallback candidates: " + std::to_string(candidates.size()));
    }

    for (const auto& candidate : candidates) {
        const auto reading = try_read_candidate(candidate.path, candidate.instance_id);
        if (!reading.has_value()) {
            continue;
        }
        PutDualShockHidRoute(cache_key, candidate.path, candidate.instance_id);
        return reading;
    }

    return std::nullopt;
}

std::optional<std::uint8_t> TryReadControllerBatteryFromGameInput(const std::string& device_name_hint,
                                                                   const std::string& device_id_hint) {
    const std::string target_name = NormalizeControllerNameForMatch(device_name_hint);
    const bool has_specific_hint = !target_name.empty() && target_name != "wirelesscontroller";

    auto matches_hint = [&](const std::string& candidate_name) {
        const std::string normalized_candidate = NormalizeControllerNameForMatch(candidate_name);
        if (normalized_candidate.empty()) {
            return false;
        }
        if (has_specific_hint) {
            return normalized_candidate.find(target_name) != std::string::npos ||
                   target_name.find(normalized_candidate) != std::string::npos;
        }
        return normalized_candidate.find("wirelesscontroller") != std::string::npos ||
               normalized_candidate.find("dualshock") != std::string::npos ||
               normalized_candidate.find("dualsense") != std::string::npos ||
               normalized_candidate.find("gamepad") != std::string::npos ||
               normalized_candidate.find("controller") != std::string::npos;
    };

    std::optional<std::uint8_t> best_fallback;
    try {
        for (const auto& controller : RawGameController::RawGameControllers()) {
            std::string candidate_name = ToUtf8(controller.DisplayName());
            if (candidate_name.empty()) {
                candidate_name = ToUtf8(controller.NonRoamableId());
            }
            if (!matches_hint(candidate_name)) {
                continue;
            }

            const auto battery_info = controller.try_as<IGameControllerBatteryInfo>();
            if (!battery_info) {
                continue;
            }
            const auto battery_report = battery_info.TryGetBatteryReport();
            const auto percent = TryConvertBatteryReportToPercent(battery_report);
            if (!percent.has_value()) {
                continue;
            }

            if (DebugEnabled()) {
                DebugLog("GameInput controller battery accepted for '" + device_name_hint +
                         "' (raw='" + candidate_name + "', idHint='" + device_id_hint +
                         "') value=" + std::to_string(*percent));
            }

            if (has_specific_hint) {
                return percent;
            }
            if (!best_fallback.has_value()) {
                best_fallback = percent;
            }
        }

        if (!best_fallback.has_value() && !has_specific_hint) {
            const auto gamepads = Gamepad::Gamepads();
            for (const auto& gamepad : gamepads) {
                const auto battery_info = gamepad.try_as<IGameControllerBatteryInfo>();
                if (!battery_info) {
                    continue;
                }
                const auto battery_report = battery_info.TryGetBatteryReport();
                const auto percent = TryConvertBatteryReportToPercent(battery_report);
                if (!percent.has_value()) {
                    continue;
                }
                if (DebugEnabled()) {
                    DebugLog("GameInput gamepad battery fallback accepted for '" + device_name_hint +
                             "' (idHint='" + device_id_hint + "') value=" + std::to_string(*percent));
                }
                best_fallback = percent;
                break;
            }
        }
    } catch (const winrt::hresult_error& error) {
        DebugLog("GameInput controller battery fallback failed: " + DescribeHresultError(error));
    }

    return best_fallback;
}

bool ShouldAggressiveXiaomiClassicRetry(const std::string& primary_name, const std::string& secondary_name,
                                        const std::string& device_id) {
    const std::string probe = ToLowerAscii(primary_name + " " + secondary_name + " " + device_id);
    const bool is_zmi_family = probe.find("zmi") != std::string::npos || probe.find("purpods") != std::string::npos;
    if (is_zmi_family) {
        return false;
    }

    return probe.find("redmi") != std::string::npos || probe.find("xiaomi") != std::string::npos ||
           probe.find("airdots") != std::string::npos;
}

void AssignFallbackComponents(std::vector<BatteryReading>* readings, bool prefer_tws_labels) {
    if (readings == nullptr || readings->empty()) {
        return;
    }

    if (readings->size() == 1 && readings->front().component.empty()) {
        readings->front().component = "main";
        return;
    }

    std::unordered_set<std::string> used;
    for (const auto& reading : *readings) {
        if (!reading.component.empty()) {
            used.insert(reading.component);
        }
    }

    constexpr std::array<const char*, 3> kPreferredComponents = {"left", "right", "case"};
    std::size_t part_index = 1;

    for (auto& reading : *readings) {
        if (!reading.component.empty()) {
            continue;
        }

        if (prefer_tws_labels) {
            bool assigned = false;
            for (const auto* preferred_component : kPreferredComponents) {
                if (!used.contains(preferred_component)) {
                    reading.component = preferred_component;
                    used.insert(preferred_component);
                    assigned = true;
                    break;
                }
            }
            if (assigned) {
                continue;
            }
        }

        reading.component = "part" + std::to_string(part_index++);
    }
}

int ComponentSortWeight(const std::string& component) {
    if (component == "left") {
        return 0;
    }
    if (component == "right") {
        return 1;
    }
    if (component == "case") {
        return 2;
    }
    if (component == "main") {
        return 3;
    }
    return 10;
}

std::string BatteryValueTag(const std::optional<std::uint8_t>& battery_level_percent) {
    if (!battery_level_percent.has_value()) {
        return "na";
    }
    return std::to_string(*battery_level_percent);
}

std::string MakeEntryKey(const DeviceBatteryInfo& entry) {
    return entry.device_id + "|" + entry.battery_component + "|" + BatteryValueTag(entry.battery_level_percent) +
           "|" + (entry.is_cached ? "cached" : "live");
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
        DebugLog("WaitForAsyncResult failed: " + DescribeHresultError(error));
        return std::nullopt;
    }
}

bool TryGetPropertyValue(const DeviceInformation& device_info, const wchar_t* property_name, IInspectable* value) {
    if (value == nullptr) {
        return false;
    }

    const auto properties = device_info.Properties();
    const winrt::hstring key(property_name);
    if (!properties.HasKey(key)) {
        return false;
    }

    *value = properties.Lookup(key);
    return *value != nullptr;
}

bool TryGetBooleanProperty(const DeviceInformation& device_info, const wchar_t* property_name, bool* value) {
    if (value == nullptr) {
        return false;
    }

    IInspectable raw_value = nullptr;
    if (!TryGetPropertyValue(device_info, property_name, &raw_value)) {
        return false;
    }

    try {
        *value = winrt::unbox_value<bool>(raw_value);
        return true;
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

std::string GuidToString(const winrt::guid& value) {
    return ToUtf8(winrt::to_hstring(value));
}

bool TryGetStringProperty(const DeviceInformation& device_info, const wchar_t* property_name, std::string* value) {
    if (value == nullptr) {
        return false;
    }

    IInspectable raw_value = nullptr;
    if (!TryGetPropertyValue(device_info, property_name, &raw_value)) {
        return false;
    }

    try {
        *value = ToUtf8(winrt::unbox_value<winrt::hstring>(raw_value));
        return !value->empty();
    } catch (const winrt::hresult_error&) {
    }

    try {
        *value = GuidToString(winrt::unbox_value<winrt::guid>(raw_value));
        return !value->empty();
    } catch (const winrt::hresult_error&) {
    }

    try {
        *value = std::to_string(winrt::unbox_value<std::uint64_t>(raw_value));
        return !value->empty();
    } catch (const winrt::hresult_error&) {
    }

    try {
        *value = std::to_string(winrt::unbox_value<std::uint32_t>(raw_value));
        return !value->empty();
    } catch (const winrt::hresult_error&) {
    }

    return false;
}

bool TryGetUInt64Property(const DeviceInformation& device_info, const wchar_t* property_name, std::uint64_t* value) {
    if (value == nullptr) {
        return false;
    }

    IInspectable raw_value = nullptr;
    if (!TryGetPropertyValue(device_info, property_name, &raw_value)) {
        return false;
    }

    try {
        *value = winrt::unbox_value<std::uint64_t>(raw_value);
        return true;
    } catch (const winrt::hresult_error&) {
    }

    try {
        *value = static_cast<std::uint64_t>(winrt::unbox_value<std::uint32_t>(raw_value));
        return true;
    } catch (const winrt::hresult_error&) {
    }

    std::string text;
    if (TryGetStringProperty(device_info, property_name, &text)) {
        try {
            *value = std::stoull(text);
            return true;
        } catch (const std::exception&) {
        }
    }

    return false;
}

bool TryGetUInt32Property(const DeviceInformation& device_info, const wchar_t* property_name, std::uint32_t* value) {
    if (value == nullptr) {
        return false;
    }

    IInspectable raw_value = nullptr;
    if (!TryGetPropertyValue(device_info, property_name, &raw_value)) {
        return false;
    }

    try {
        *value = winrt::unbox_value<std::uint32_t>(raw_value);
        return true;
    } catch (const winrt::hresult_error&) {
    }

    try {
        *value = static_cast<std::uint32_t>(winrt::unbox_value<std::uint16_t>(raw_value));
        return true;
    } catch (const winrt::hresult_error&) {
    }

    try {
        *value = static_cast<std::uint32_t>(winrt::unbox_value<std::uint64_t>(raw_value));
        return true;
    } catch (const winrt::hresult_error&) {
    }

    try {
        const auto signed_value = winrt::unbox_value<std::int32_t>(raw_value);
        if (signed_value >= 0) {
            *value = static_cast<std::uint32_t>(signed_value);
            return true;
        }
    } catch (const winrt::hresult_error&) {
    }

    std::string text;
    if (TryGetStringProperty(device_info, property_name, &text)) {
        try {
            *value = static_cast<std::uint32_t>(std::stoul(text));
            return true;
        } catch (const std::exception&) {
        }
    }

    return false;
}

bool TryGetStringArrayProperty(
    const DeviceInformation& device_info,
    const wchar_t* property_name,
    std::vector<std::string>* values) {
    if (values == nullptr) {
        return false;
    }

    IInspectable raw_value = nullptr;
    if (!TryGetPropertyValue(device_info, property_name, &raw_value)) {
        return false;
    }

    if (const auto property_value = raw_value.try_as<IPropertyValue>()) {
        try {
            if (property_value.Type() == PropertyType::StringArray) {
                winrt::com_array<winrt::hstring> array_values;
                property_value.GetStringArray(array_values);
                values->clear();
                values->reserve(array_values.size());
                for (const auto& text : array_values) {
                    const std::string utf8 = ToUtf8(text);
                    if (!utf8.empty()) {
                        values->push_back(utf8);
                    }
                }
                return !values->empty();
            }
        } catch (const winrt::hresult_error&) {
        }
    }

    std::string single_value;
    if (TryGetStringProperty(device_info, property_name, &single_value) && !single_value.empty()) {
        values->assign(1U, single_value);
        return true;
    }

    return false;
}

void AppendUniqueStrings(std::vector<std::string>* target, const std::vector<std::string>& incoming) {
    if (target == nullptr) {
        return;
    }
    for (const auto& value : incoming) {
        if (value.empty()) {
            continue;
        }
        const auto it = std::find(target->begin(), target->end(), value);
        if (it == target->end()) {
            target->push_back(value);
        }
    }
}

void PopulateBluetoothVisualHintsFromDeviceInfo(const DeviceInformation& device_info, DeviceBatteryInfo* entry) {
    if (entry == nullptr) {
        return;
    }

    std::uint32_t value32 = 0;
    if (TryGetUInt32Property(device_info, L"System.Devices.Aep.Bluetooth.Le.Appearance", &value32)) {
        entry->bluetooth_le_appearance = static_cast<std::uint16_t>(value32);
    }
    if (TryGetUInt32Property(device_info, L"System.Devices.Aep.Bluetooth.Cod.Major", &value32)) {
        entry->bluetooth_cod_major = value32;
    }
    if (TryGetUInt32Property(device_info, L"System.Devices.Aep.Bluetooth.Cod.Minor", &value32)) {
        entry->bluetooth_cod_minor = value32;
    }

    std::vector<std::string> categories;
    if (TryGetStringArrayProperty(device_info, kDeviceContainerCategoryProperty, &categories)) {
        AppendUniqueStrings(&entry->device_categories, categories);
    }
    if (TryGetStringArrayProperty(device_info, kDeviceContainerPrimaryCategoryProperty, &categories)) {
        AppendUniqueStrings(&entry->device_categories, categories);
    }
    if (TryGetStringArrayProperty(device_info, L"System.Devices.AepContainer.Categories", &categories)) {
        AppendUniqueStrings(&entry->device_categories, categories);
    }
    if (TryGetStringArrayProperty(device_info, L"System.Devices.Aep.Category", &categories)) {
        AppendUniqueStrings(&entry->device_categories, categories);
    }
    if (TryGetStringArrayProperty(device_info, L"System.Devices.Category", &categories)) {
        AppendUniqueStrings(&entry->device_categories, categories);
    }
}

void PopulateBluetoothVisualHintsFromEndpointCandidate(const EndpointCandidate& candidate, DeviceBatteryInfo* entry) {
    if (entry == nullptr) {
        return;
    }
    if (!entry->bluetooth_le_appearance.has_value() && candidate.bluetooth_le_appearance.has_value()) {
        entry->bluetooth_le_appearance = candidate.bluetooth_le_appearance;
    }
    if (!entry->bluetooth_cod_major.has_value() && candidate.bluetooth_cod_major.has_value()) {
        entry->bluetooth_cod_major = candidate.bluetooth_cod_major;
    }
    if (!entry->bluetooth_cod_minor.has_value() && candidate.bluetooth_cod_minor.has_value()) {
        entry->bluetooth_cod_minor = candidate.bluetooth_cod_minor;
    }
    AppendUniqueStrings(&entry->device_categories, candidate.device_categories);
}

void PopulateBluetoothVisualHintsFromDeviceInfo(const DeviceInformation& device_info, EndpointCandidate* candidate) {
    if (candidate == nullptr) {
        return;
    }

    std::uint32_t value32 = 0;
    if (TryGetUInt32Property(device_info, L"System.Devices.Aep.Bluetooth.Le.Appearance", &value32)) {
        candidate->bluetooth_le_appearance = static_cast<std::uint16_t>(value32);
    }
    if (TryGetUInt32Property(device_info, L"System.Devices.Aep.Bluetooth.Cod.Major", &value32)) {
        candidate->bluetooth_cod_major = value32;
    }
    if (TryGetUInt32Property(device_info, L"System.Devices.Aep.Bluetooth.Cod.Minor", &value32)) {
        candidate->bluetooth_cod_minor = value32;
    }

    std::vector<std::string> categories;
    if (TryGetStringArrayProperty(device_info, kDeviceContainerCategoryProperty, &categories)) {
        AppendUniqueStrings(&candidate->device_categories, categories);
    }
    if (TryGetStringArrayProperty(device_info, kDeviceContainerPrimaryCategoryProperty, &categories)) {
        AppendUniqueStrings(&candidate->device_categories, categories);
    }
    if (TryGetStringArrayProperty(device_info, L"System.Devices.AepContainer.Categories", &categories)) {
        AppendUniqueStrings(&candidate->device_categories, categories);
    }
    if (TryGetStringArrayProperty(device_info, L"System.Devices.Aep.Category", &categories)) {
        AppendUniqueStrings(&candidate->device_categories, categories);
    }
    if (TryGetStringArrayProperty(device_info, L"System.Devices.Category", &categories)) {
        AppendUniqueStrings(&candidate->device_categories, categories);
    }
}

std::optional<std::uint8_t> ParsePercentFromText(const std::string& text) {
    int current_number = -1;
    for (const char ch : text) {
        if (ch >= '0' && ch <= '9') {
            if (current_number < 0) {
                current_number = 0;
            }
            current_number = (current_number * 10) + (ch - '0');
            continue;
        }

        if (current_number >= 0) {
            break;
        }
    }

    if (current_number < 0 || current_number > 100) {
        return std::nullopt;
    }

    return static_cast<std::uint8_t>(current_number);
}

std::optional<std::uint64_t> ParseBluetoothAddress(const std::string& value) {
    std::string hex;
    hex.reserve(value.size());

    for (const char ch : value) {
        const bool is_decimal = (ch >= '0' && ch <= '9');
        const bool is_lower_hex = (ch >= 'a' && ch <= 'f');
        const bool is_upper_hex = (ch >= 'A' && ch <= 'F');
        if (is_decimal || is_lower_hex || is_upper_hex) {
            hex.push_back(ch);
        }
    }

    if (hex.size() != 12) {
        return std::nullopt;
    }

    try {
        return std::stoull(hex, nullptr, 16);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<std::uint64_t> ParseBluetoothAddressesFromDeviceId(const std::string& device_id) {
    const std::string lowered = ToLowerAscii(device_id);
    std::vector<std::uint64_t> addresses;

    auto try_parse_window = [&lowered](std::size_t start) -> std::optional<std::uint64_t> {
        if (start + 17 > lowered.size()) {
            return std::nullopt;
        }
        const std::string candidate = lowered.substr(start, 17);
        return ParseBluetoothAddress(candidate);
    };

    for (std::size_t i = 0; i < lowered.size(); ++i) {
        if (i + 17 <= lowered.size() && lowered[i + 2] == ':' && lowered[i + 5] == ':' && lowered[i + 8] == ':' &&
            lowered[i + 11] == ':' && lowered[i + 14] == ':') {
            const auto parsed = try_parse_window(i);
            if (parsed.has_value()) {
                if (std::find(addresses.begin(), addresses.end(), *parsed) == addresses.end()) {
                    addresses.push_back(*parsed);
                }
            }
        }
    }

    for (std::size_t i = 0; i + 16 <= lowered.size(); ++i) {
        if (lowered.compare(i, 4, "dev_") != 0) {
            continue;
        }

        const std::string candidate = lowered.substr(i + 4, 12);
        bool is_hex = true;
        for (const char ch : candidate) {
            const bool is_decimal = (ch >= '0' && ch <= '9');
            const bool is_lower_hex = (ch >= 'a' && ch <= 'f');
            if (!is_decimal && !is_lower_hex) {
                is_hex = false;
                break;
            }
        }

        if (!is_hex) {
            continue;
        }

        try {
            const auto parsed = std::stoull(candidate, nullptr, 16);
            if (std::find(addresses.begin(), addresses.end(), parsed) == addresses.end()) {
                addresses.push_back(parsed);
            }
        } catch (const std::exception&) {
        }
    }

    return addresses;
}

std::optional<std::uint64_t> ParseBluetoothAddressFromDeviceId(const std::string& device_id) {
    auto addresses = ParseBluetoothAddressesFromDeviceId(device_id);
    if (addresses.empty()) {
        return std::nullopt;
    }

    // Device ID may include both adapter and remote addresses; the remote one is usually the last segment.
    return addresses.back();
}

std::optional<std::uint16_t> ReadBluetoothProductIdFromRegistry(std::uint64_t address) {
    if (address <= 0xFFFFULL) {
        return std::nullopt;
    }

    std::wstringstream key_builder;
    key_builder << L"SYSTEM\\CurrentControlSet\\Services\\BTHPORT\\Parameters\\Devices\\"
                << std::nouppercase << std::hex
                << std::setw(12) << std::setfill(L'0') << address;
    const std::wstring key_path = key_builder.str();

    HKEY key_handle = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_path.c_str(), 0, KEY_QUERY_VALUE, &key_handle) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    DWORD value_type = 0;
    DWORD value = 0;
    DWORD value_size = sizeof(value);
    const auto query_result = RegQueryValueExW(key_handle, L"PID", nullptr, &value_type,
                                               reinterpret_cast<LPBYTE>(&value), &value_size);
    RegCloseKey(key_handle);

    if (query_result != ERROR_SUCCESS || value_size < sizeof(DWORD) || value_type != REG_DWORD) {
        return std::nullopt;
    }
    if (value > 0xFFFFU) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

std::vector<std::wstring> FindBthEnumInstanceIdsByAddress(std::uint64_t address) {
    std::wstringstream prefix_builder;
    prefix_builder << L"BTHENUM\\DEV_" << std::uppercase << std::hex
                   << std::setw(12) << std::setfill(L'0') << address;
    const std::wstring expected_prefix = prefix_builder.str();

    std::wstringstream filter_builder;
    filter_builder << expected_prefix << L"\\*";
    const std::wstring filter = filter_builder.str();

    ULONG size = 0;
    const auto size_result =
        CM_Get_Device_ID_List_SizeW(&size, filter.c_str(), CM_GETIDLIST_FILTER_NONE);
    if (size_result != CR_SUCCESS || size <= 1U) {
        return {};
    }

    std::vector<wchar_t> buffer(size);
    const auto list_result =
        CM_Get_Device_ID_ListW(filter.c_str(), buffer.data(), size, CM_GETIDLIST_FILTER_NONE);
    if (list_result != CR_SUCCESS || buffer.empty() || buffer.front() == L'\0') {
        return {};
    }

    std::vector<std::wstring> instance_ids;
    std::size_t cursor = 0;
    while (cursor < buffer.size() && buffer[cursor] != L'\0') {
        const wchar_t* current = buffer.data() + static_cast<std::ptrdiff_t>(cursor);
        const std::size_t length = std::wcslen(current);
        if (length == 0U) {
            break;
        }
        std::wstring instance_id(current, length);
        std::wstring normalized = instance_id;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](wchar_t value) {
                           return static_cast<wchar_t>(std::towupper(value));
                       });
        if (normalized.rfind(expected_prefix, 0) == 0) {
            instance_ids.push_back(std::move(instance_id));
        }
        cursor += length + 1U;
    }

    return instance_ids;
}

std::vector<std::wstring> FindBthEnumServiceInstanceIdsByAddress(std::uint64_t address,
                                                                 const std::wstring& service_uuid_upper) {
    if (address <= 0xFFFFULL || service_uuid_upper.empty()) {
        return {};
    }

    std::wstringstream address_builder;
    address_builder << std::uppercase << std::hex << std::setw(12) << std::setfill(L'0') << address;
    const std::wstring address_token = address_builder.str();

    std::wstringstream filter_builder;
    filter_builder << L"BTHENUM\\*" << address_token << L"*";
    const std::wstring filter = filter_builder.str();

    ULONG size = 0;
    const auto size_result = CM_Get_Device_ID_List_SizeW(&size, filter.c_str(), CM_GETIDLIST_FILTER_NONE);
    if (size_result != CR_SUCCESS || size <= 1U) {
        return {};
    }

    std::vector<wchar_t> buffer(size);
    const auto list_result = CM_Get_Device_ID_ListW(filter.c_str(), buffer.data(), size, CM_GETIDLIST_FILTER_NONE);
    if (list_result != CR_SUCCESS || buffer.empty() || buffer.front() == L'\0') {
        return {};
    }

    const std::wstring expected_service_prefix = L"BTHENUM\\{" + service_uuid_upper + L"}";
    std::vector<std::wstring> instance_ids;
    std::size_t cursor = 0;
    while (cursor < buffer.size() && buffer[cursor] != L'\0') {
        const wchar_t* current = buffer.data() + static_cast<std::ptrdiff_t>(cursor);
        const std::size_t length = std::wcslen(current);
        if (length == 0U) {
            break;
        }

        std::wstring instance_id(current, length);
        std::wstring normalized = instance_id;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](wchar_t value) { return static_cast<wchar_t>(std::towupper(value)); });

        if (normalized.rfind(expected_service_prefix, 0) == 0 &&
            normalized.find(address_token) != std::wstring::npos) {
            instance_ids.push_back(std::move(instance_id));
        }
        cursor += length + 1U;
    }

    return instance_ids;
}

std::vector<std::wstring> FindBthLeInstanceIdsByAddress(std::uint64_t address) {
    std::wstringstream prefix_builder;
    prefix_builder << L"BTHLE\\DEV_" << std::uppercase << std::hex
                   << std::setw(12) << std::setfill(L'0') << address;
    const std::wstring expected_prefix = prefix_builder.str();

    std::wstringstream filter_builder;
    filter_builder << expected_prefix << L"\\*";
    const std::wstring filter = filter_builder.str();

    ULONG size = 0;
    const auto size_result =
        CM_Get_Device_ID_List_SizeW(&size, filter.c_str(), CM_GETIDLIST_FILTER_NONE);
    if (size_result != CR_SUCCESS || size <= 1U) {
        return {};
    }

    std::vector<wchar_t> buffer(size);
    const auto list_result =
        CM_Get_Device_ID_ListW(filter.c_str(), buffer.data(), size, CM_GETIDLIST_FILTER_NONE);
    if (list_result != CR_SUCCESS || buffer.empty() || buffer.front() == L'\0') {
        return {};
    }

    std::vector<std::wstring> instance_ids;
    std::size_t cursor = 0;
    while (cursor < buffer.size() && buffer[cursor] != L'\0') {
        const wchar_t* current = buffer.data() + static_cast<std::ptrdiff_t>(cursor);
        const std::size_t length = std::wcslen(current);
        if (length == 0U) {
            break;
        }
        std::wstring instance_id(current, length);
        std::wstring normalized = instance_id;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](wchar_t value) {
                           return static_cast<wchar_t>(std::towupper(value));
                       });
        if (normalized.rfind(expected_prefix, 0) == 0) {
            instance_ids.push_back(std::move(instance_id));
        }
        cursor += length + 1U;
    }

    return instance_ids;
}

std::optional<std::vector<std::uint8_t>> ReadDevNodePropertyRaw(DEVINST dev_inst,
                                                                const DEVPROPKEY& property_key,
                                                                DEVPROPTYPE* property_type) {
    if (property_type == nullptr) {
        return std::nullopt;
    }

    *property_type = DEVPROP_TYPE_EMPTY;
    ULONG size = 0;
    const auto query_result =
        CM_Get_DevNode_PropertyW(dev_inst, &property_key, property_type, nullptr, &size, 0);
    if (query_result != CR_SUCCESS && query_result != CR_BUFFER_SMALL) {
        return std::nullopt;
    }
    if (size == 0U) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> raw_data(size);
    const auto property_result = CM_Get_DevNode_PropertyW(
        dev_inst, &property_key, property_type, reinterpret_cast<PBYTE>(raw_data.data()), &size, 0);
    if (property_result != CR_SUCCESS) {
        return std::nullopt;
    }
    raw_data.resize(size);
    return raw_data;
}

std::optional<std::uint32_t> ReadDevNodeUInt32Property(DEVINST dev_inst, const DEVPROPKEY& property_key) {
    DEVPROPTYPE property_type = DEVPROP_TYPE_EMPTY;
    const auto raw_data = ReadDevNodePropertyRaw(dev_inst, property_key, &property_type);
    if (!raw_data.has_value()) {
        return std::nullopt;
    }

    if (property_type == DEVPROP_TYPE_UINT32 && raw_data->size() >= sizeof(std::uint32_t)) {
        std::uint32_t value = 0;
        std::memcpy(&value, raw_data->data(), sizeof(value));
        return value;
    }
    if (property_type == DEVPROP_TYPE_INT32 && raw_data->size() >= sizeof(std::int32_t)) {
        std::int32_t value = 0;
        std::memcpy(&value, raw_data->data(), sizeof(value));
        if (value >= 0) {
            return static_cast<std::uint32_t>(value);
        }
    }
    return std::nullopt;
}

std::vector<std::string> ReadDevNodeStringListProperty(DEVINST dev_inst, const DEVPROPKEY& property_key) {
    DEVPROPTYPE property_type = DEVPROP_TYPE_EMPTY;
    const auto raw_data = ReadDevNodePropertyRaw(dev_inst, property_key, &property_type);
    if (!raw_data.has_value()) {
        return {};
    }

    std::vector<std::string> values;
    if ((property_type & DEVPROP_MASK_TYPE) == DEVPROP_TYPE_STRING && raw_data->size() >= sizeof(wchar_t)) {
        const wchar_t* cursor = reinterpret_cast<const wchar_t*>(raw_data->data());
        const wchar_t* end = reinterpret_cast<const wchar_t*>(raw_data->data() + raw_data->size());
        while (cursor < end && *cursor != L'\0') {
            const std::size_t length = std::wcslen(cursor);
            if (length == 0U) {
                break;
            }
            values.push_back(ToUtf8(std::wstring(cursor, length)));
            cursor += length + 1U;
        }
    }
    return values;
}

std::optional<PnpBluetoothVisualHints> ReadBluetoothVisualHintsFromPnpAddress(std::uint64_t address) {
    if (address <= 0xFFFFULL) {
        return std::nullopt;
    }

    const auto instance_ids = FindBthEnumInstanceIdsByAddress(address);
    std::vector<std::wstring> ranked_instance_ids = instance_ids;
    const auto bthle_instance_ids = FindBthLeInstanceIdsByAddress(address);
    ranked_instance_ids.insert(ranked_instance_ids.end(), bthle_instance_ids.begin(), bthle_instance_ids.end());
    if (ranked_instance_ids.empty()) {
        return std::nullopt;
    }

    std::wstringstream prefix_builder;
    prefix_builder << L"BTHENUM\\DEV_" << std::uppercase << std::hex
                   << std::setw(12) << std::setfill(L'0') << address;
    const std::wstring preferred_prefix = prefix_builder.str();
    std::stable_sort(ranked_instance_ids.begin(), ranked_instance_ids.end(),
                     [&preferred_prefix](const std::wstring& lhs, const std::wstring& rhs) {
                         auto score = [&preferred_prefix](const std::wstring& value) {
                             int rank = 0;
                             if (value.rfind(preferred_prefix, 0) == 0) {
                                 rank += 100;
                             }
                             if (value.rfind(L"BTHLE\\DEV_", 0) == 0) {
                                 rank += 90;
                             }
                             if (value.find(L"\\DEV_") != std::wstring::npos) {
                                 rank += 20;
                             }
                             if (value.find(L"{") != std::wstring::npos) {
                                 rank -= 12;
                             }
                             return rank;
                         };
                         return score(lhs) > score(rhs);
                     });

    for (const auto& instance_id : ranked_instance_ids) {
        DEVINST dev_inst = 0;
        const auto locate_result =
            CM_Locate_DevNodeW(&dev_inst, const_cast<wchar_t*>(instance_id.c_str()), CM_LOCATE_DEVNODE_NORMAL);
        if (locate_result != CR_SUCCESS) {
            continue;
        }

        PnpBluetoothVisualHints hints;
        if (const auto class_of_device = ReadDevNodeUInt32Property(dev_inst, kBluetoothClassOfDevicePropKey);
            class_of_device.has_value()) {
            hints.bluetooth_cod_major = ((*class_of_device >> 8U) & 0x1FU);
            hints.bluetooth_cod_minor = ((*class_of_device >> 2U) & 0x3FU);
        }
        AppendUniqueStrings(&hints.device_categories, ReadDevNodeStringListProperty(dev_inst, kDeviceContainerCategoryPropKey));
        AppendUniqueStrings(
            &hints.device_categories, ReadDevNodeStringListProperty(dev_inst, kDeviceContainerPrimaryCategoryPropKey));

        if (hints.bluetooth_cod_major.has_value() || !hints.device_categories.empty()) {
            return hints;
        }
    }

    return std::nullopt;
}

std::optional<std::uint8_t> NormalizePhoneBatteryHintScalar(int raw_value) {
    if (raw_value < 0 || raw_value == 255) {
        return std::nullopt;
    }
    if (raw_value <= 100) {
        return static_cast<std::uint8_t>(raw_value);
    }
    if ((raw_value & 0xFF) == raw_value) {
        const int masked = raw_value & 0x7F;
        if (masked >= 0 && masked <= 100) {
            return static_cast<std::uint8_t>(masked);
        }
    }
    return std::nullopt;
}

std::optional<std::uint8_t> ReadPhoneHfpBatteryHintFromPnpAddress(std::uint64_t address) {
    if (address <= 0xFFFFULL) {
        return std::nullopt;
    }

    std::vector<std::wstring> instance_ids;
    std::unordered_set<std::wstring> seen_instance_ids;
    auto append_instance_ids = [&](const std::vector<std::wstring>& ids) {
        for (const auto& instance_id : ids) {
            std::wstring normalized = instance_id;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                           [](wchar_t value) { return static_cast<wchar_t>(std::towupper(value)); });
            if (seen_instance_ids.insert(std::move(normalized)).second) {
                instance_ids.push_back(instance_id);
            }
        }
    };

    constexpr std::array<const wchar_t*, 3> kServiceUuids = {
        L"0000111F-0000-1000-8000-00805F9B34FB",
        L"0000111E-0000-1000-8000-00805F9B34FB",
        L"00001112-0000-1000-8000-00805F9B34FB",
    };
    for (const auto* service_uuid : kServiceUuids) {
        append_instance_ids(FindBthEnumServiceInstanceIdsByAddress(address, service_uuid));
    }

    // Some drivers only expose DEV_* nodes; keep this as a last-resort fallback.
    append_instance_ids(FindBthEnumInstanceIdsByAddress(address));

    if (instance_ids.empty()) {
        DebugLog("Phone PnP battery hint: instance id not found for address=" + std::to_string(address));
        return std::nullopt;
    }

    auto read_property = [&](DEVINST dev_inst, const DEVPROPKEY& property_key) -> std::optional<std::uint8_t> {
        DEVPROPTYPE property_type = DEVPROP_TYPE_EMPTY;
        ULONG size = 0;
        const auto query_result =
            CM_Get_DevNode_PropertyW(dev_inst, &property_key, &property_type, nullptr, &size, 0);
        if (query_result != CR_SUCCESS && query_result != CR_BUFFER_SMALL) {
            return std::nullopt;
        }
        if (size == 0U) {
            return std::nullopt;
        }

        std::vector<std::uint8_t> raw_data(size);
        const auto property_result = CM_Get_DevNode_PropertyW(
            dev_inst, &property_key, &property_type, reinterpret_cast<PBYTE>(raw_data.data()), &size, 0);
        if (property_result != CR_SUCCESS) {
            return std::nullopt;
        }
        raw_data.resize(size);

        if (DebugEnabled()) {
            const std::size_t preview_len = std::min<std::size_t>(raw_data.size(), 24U);
            std::vector<std::uint8_t> preview(raw_data.begin(),
                                              raw_data.begin() + static_cast<std::ptrdiff_t>(preview_len));
            DebugLog("Phone PnP raw pid=" + std::to_string(property_key.pid) +
                     " type=" + std::to_string(property_type) +
                     " size=" + std::to_string(raw_data.size()) +
                     " data=" + BytesToHex(preview));
        }

        if (property_type == DEVPROP_TYPE_BYTE && !raw_data.empty()) {
            return NormalizePhoneBatteryHintScalar(static_cast<int>(raw_data[0]));
        }
        if (property_type == DEVPROP_TYPE_UINT16 && size >= sizeof(std::uint16_t)) {
            std::uint16_t value = 0;
            std::memcpy(&value, raw_data.data(), sizeof(value));
            return NormalizePhoneBatteryHintScalar(static_cast<int>(value));
        }
        if (property_type == DEVPROP_TYPE_INT16 && size >= sizeof(std::int16_t)) {
            std::int16_t value = 0;
            std::memcpy(&value, raw_data.data(), sizeof(value));
            return NormalizePhoneBatteryHintScalar(static_cast<int>(value));
        }
        if (property_type == DEVPROP_TYPE_UINT32 && size >= sizeof(std::uint32_t)) {
            std::uint32_t value = 0;
            std::memcpy(&value, raw_data.data(), sizeof(value));
            if (value <= static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
                return NormalizePhoneBatteryHintScalar(static_cast<int>(value));
            }
            return std::nullopt;
        }
        if (property_type == DEVPROP_TYPE_INT32 && size >= sizeof(std::int32_t)) {
            std::int32_t value = 0;
            std::memcpy(&value, raw_data.data(), sizeof(value));
            return NormalizePhoneBatteryHintScalar(static_cast<int>(value));
        }
        if ((property_type & DEVPROP_MASK_TYPE) == DEVPROP_TYPE_STRING && size >= sizeof(wchar_t)) {
            std::wstring text(reinterpret_cast<const wchar_t*>(raw_data.data()),
                              raw_data.size() / sizeof(wchar_t));
            if (const auto null_pos = text.find(L'\0'); null_pos != std::wstring::npos) {
                text.resize(null_pos);
            }

            const wchar_t* cursor = text.c_str();
            while (*cursor != L'\0') {
                while (*cursor != L'\0' && !std::iswdigit(*cursor) && *cursor != L'-') {
                    ++cursor;
                }
                if (*cursor == L'\0') {
                    break;
                }
                wchar_t* end = nullptr;
                const long value = std::wcstol(cursor, &end, 10);
                if (end != cursor) {
                    return NormalizePhoneBatteryHintScalar(static_cast<int>(value));
                }
                cursor = end;
            }
        }

        return std::nullopt;
    };

    for (const auto& instance_id : instance_ids) {
        DEVINST dev_inst = 0;
        const auto locate_result =
            CM_Locate_DevNodeW(&dev_inst, const_cast<wchar_t*>(instance_id.c_str()), CM_LOCATE_DEVNODE_NORMAL);
        if (locate_result != CR_SUCCESS) {
            continue;
        }

        std::vector<DEVPROPKEY> keys_to_probe = {kPhoneHfpBatteryHintPropKey};

        ULONG key_count = 0;
        const auto key_count_result = CM_Get_DevNode_Property_Keys(dev_inst, nullptr, &key_count, 0);
        if ((key_count_result == CR_SUCCESS || key_count_result == CR_BUFFER_SMALL) && key_count > 0U) {
            std::vector<DEVPROPKEY> property_keys(key_count);
            if (CM_Get_DevNode_Property_Keys(dev_inst, property_keys.data(), &key_count, 0) == CR_SUCCESS) {
                property_keys.resize(key_count);
                for (const auto& property_key : property_keys) {
                    if (std::memcmp(&property_key.fmtid, &kPhoneHfpBatteryHintPropKey.fmtid, sizeof(GUID)) != 0) {
                        continue;
                    }
                    const bool already_added = std::any_of(
                        keys_to_probe.begin(), keys_to_probe.end(),
                        [&property_key](const DEVPROPKEY& existing) {
                            return std::memcmp(&existing, &property_key, sizeof(DEVPROPKEY)) == 0;
                        });
                    if (!already_added) {
                        keys_to_probe.push_back(property_key);
                    }
                }
            }
        }

        std::sort(keys_to_probe.begin(), keys_to_probe.end(),
                  [](const DEVPROPKEY& lhs, const DEVPROPKEY& rhs) {
                      return lhs.pid < rhs.pid;
                  });
        keys_to_probe.erase(
            std::unique(keys_to_probe.begin(), keys_to_probe.end(),
                        [](const DEVPROPKEY& lhs, const DEVPROPKEY& rhs) {
                            return std::memcmp(&lhs, &rhs, sizeof(DEVPROPKEY)) == 0;
                        }),
            keys_to_probe.end());

        for (const auto& property_key : keys_to_probe) {
            const auto value = read_property(dev_inst, property_key);
            if (!value.has_value()) {
                continue;
            }
            if (DebugEnabled()) {
                DebugLog("Phone PnP battery hint accepted value=" + std::to_string(*value) +
                         " pid=" + std::to_string(property_key.pid) +
                         " instance='" + ToUtf8(winrt::hstring(instance_id)) + "'");
            }
            return value;
        }
    }

    DebugLog("Phone PnP battery hint was not found for address=" + std::to_string(address));
    return std::nullopt;
}

std::optional<std::uint8_t> NormalizeZmiVendorBatteryScalar(int raw_value) {
    if (raw_value <= 0 || raw_value == 255) {
        return std::nullopt;
    }
    if (raw_value <= 10) {
        return static_cast<std::uint8_t>(raw_value * 10);
    }
    if (raw_value <= 100) {
        return static_cast<std::uint8_t>(raw_value);
    }
    if ((raw_value & 0xFF) == raw_value) {
        const int masked = raw_value & 0x7F;
        if (masked > 0 && masked <= 100) {
            return static_cast<std::uint8_t>(masked);
        }
    }
    return std::nullopt;
}

std::vector<BatteryReading> BuildReadingsFromTriplet(const XiaomiBatterySnapshot& snapshot) {
    std::vector<BatteryReading> readings;
    if (snapshot.left.has_value()) {
        readings.push_back(BatteryReading{"left", *snapshot.left});
    }
    if (snapshot.right.has_value()) {
        readings.push_back(BatteryReading{"right", *snapshot.right});
    }
    if (snapshot.case_level.has_value()) {
        readings.push_back(BatteryReading{"case", *snapshot.case_level});
    }
    return readings;
}

std::size_t XiaomiResolvedTwsComponentCount(const std::vector<BatteryReading>& readings) {
    bool has_left = false;
    bool has_right = false;
    bool has_case = false;

    for (const auto& reading : readings) {
        const std::string component = ToLowerAscii(reading.component);
        if (component == "left") {
            has_left = true;
        } else if (component == "right") {
            has_right = true;
        } else if (component == "case") {
            has_case = true;
        }
    }

    std::size_t count = 0;
    if (has_left) {
        ++count;
    }
    if (has_right) {
        ++count;
    }
    if (has_case) {
        ++count;
    }
    return count;
}

bool HasUsefulXiaomiTwsReadings(const std::vector<BatteryReading>& readings, std::size_t min_components = 2U) {
    return XiaomiResolvedTwsComponentCount(readings) >= min_components;
}

int XiaomiReadingsRichnessScore(const std::vector<BatteryReading>& readings) {
    const auto tws_components = static_cast<int>(XiaomiResolvedTwsComponentCount(readings));
    int score = tws_components * 100;
    for (const auto& reading : readings) {
        const std::string component = ToLowerAscii(reading.component);
        if (component == "main") {
            score += 10;
        }
    }
    score += static_cast<int>(std::min<std::size_t>(readings.size(), 9U));
    return score;
}

std::vector<BatteryReading> DecodeZmiVendorBatteryReadingsFromUInt32(std::uint32_t raw_value);

int ZmiSnapshotPresenceCount(const XiaomiBatterySnapshot& snapshot) {
    int count = 0;
    if (snapshot.left.has_value()) {
        ++count;
    }
    if (snapshot.right.has_value()) {
        ++count;
    }
    if (snapshot.case_level.has_value()) {
        ++count;
    }
    return count;
}

std::vector<BatteryReading> DecodeZmiVendorReadingsFromNumericValues(const std::vector<int>& values) {
    struct Candidate {
        XiaomiBatterySnapshot snapshot;
        int score = -1;
        int presence = 0;
    };

    if (values.empty()) {
        return {};
    }

    auto evaluate_triplet = [](int left_raw, int right_raw, int case_raw, int pattern_bonus) -> Candidate {
        XiaomiBatterySnapshot snapshot;
        snapshot.left = NormalizeZmiVendorBatteryScalar(left_raw);
        snapshot.right = NormalizeZmiVendorBatteryScalar(right_raw);
        snapshot.case_level = NormalizeZmiVendorBatteryScalar(case_raw);

        const int presence = ZmiSnapshotPresenceCount(snapshot);
        if (presence < 2) {
            return Candidate{snapshot, -1, presence};
        }

        int score = pattern_bonus;
        score += presence * 16;
        if (presence == 3) {
            score += 8;
        }
        if (snapshot.left.has_value() && snapshot.right.has_value() && *snapshot.left != *snapshot.right) {
            score += 6;
        }
        if ((snapshot.left.has_value() && *snapshot.left > 10U) ||
            (snapshot.right.has_value() && *snapshot.right > 10U) ||
            (snapshot.case_level.has_value() && *snapshot.case_level > 10U)) {
            score += 5;
        }
        if (snapshot.left.has_value() && snapshot.right.has_value() && snapshot.case_level.has_value() &&
            *snapshot.left == *snapshot.right && *snapshot.right == *snapshot.case_level) {
            score -= 7;
        }

        return Candidate{snapshot, score, presence};
    };

    std::optional<Candidate> best;
    auto consider = [&](int left_raw, int right_raw, int case_raw, int pattern_bonus) {
        const auto candidate = evaluate_triplet(left_raw, right_raw, case_raw, pattern_bonus);
        if (candidate.score < 0) {
            return;
        }
        if (!best.has_value() || candidate.score > best->score) {
            best = candidate;
        }
    };

    for (std::size_t index = 0; index + 2U < values.size(); ++index) {
        int pattern_bonus = 0;
        if (index >= 4U && values[index - 4U] == 0x02 && values[index - 3U] == 0x02 &&
            values[index - 2U] == 0x04 && values[index - 1U] == 0x07) {
            pattern_bonus += 30;
        }
        if (index >= 2U && values[index - 2U] >= 4 && values[index - 2U] <= 8 &&
            values[index - 1U] >= 0 && values[index - 1U] <= 0x7F) {
            pattern_bonus += 18;
        }

        consider(values[index], values[index + 1U], values[index + 2U], pattern_bonus);
    }

    if (best.has_value()) {
        const int threshold = best->presence == 3 ? 58 : 50;
        if (best->score >= threshold) {
            return BuildReadingsFromTriplet(best->snapshot);
        }
    }

    for (const auto raw : values) {
        const auto scalar = NormalizeZmiVendorBatteryScalar(raw);
        if (scalar.has_value()) {
            return {BatteryReading{"main", *scalar}};
        }
    }

    return {};
}

std::vector<BatteryReading> DecodeZmiVendorReadingsFromByteBlob(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return {};
    }

    std::vector<int> values;
    values.reserve(bytes.size());
    for (const auto value : bytes) {
        values.push_back(static_cast<int>(value));
    }

    auto decoded = DecodeZmiVendorReadingsFromNumericValues(values);
    if (decoded.size() >= 2U) {
        return decoded;
    }

    if (bytes.size() >= sizeof(std::uint32_t)) {
        std::vector<BatteryReading> best_packed = decoded;
        for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= bytes.size(); ++offset) {
            std::uint32_t packed = 0;
            std::memcpy(&packed, bytes.data() + static_cast<std::ptrdiff_t>(offset), sizeof(packed));
            auto packed_decoded = DecodeZmiVendorBatteryReadingsFromUInt32(packed);
            if (packed_decoded.size() > best_packed.size()) {
                best_packed = std::move(packed_decoded);
            }
        }
        if (!best_packed.empty()) {
            return best_packed;
        }
    }

    return decoded;
}

std::vector<BatteryReading> DecodeZmiVendorBatteryReadingsFromUInt32(std::uint32_t raw_value) {
    struct Candidate {
        XiaomiBatterySnapshot snapshot;
        int score = -1;
        int presence = 0;
    };

    auto evaluate_triplet = [](std::uint8_t left_raw, std::uint8_t right_raw, std::uint8_t case_raw) -> Candidate {
        XiaomiBatterySnapshot snapshot;
        snapshot.left = NormalizeZmiVendorBatteryScalar(left_raw);
        snapshot.right = NormalizeZmiVendorBatteryScalar(right_raw);
        snapshot.case_level = NormalizeZmiVendorBatteryScalar(case_raw);

        int presence = 0;
        if (snapshot.left.has_value()) {
            ++presence;
        }
        if (snapshot.right.has_value()) {
            ++presence;
        }
        if (snapshot.case_level.has_value()) {
            ++presence;
        }

        int score = 0;
        score += presence * 20;
        if (presence == 3) {
            score += 30;
        }
        if (snapshot.left.has_value() && snapshot.right.has_value() && *snapshot.left != *snapshot.right) {
            score += 8;
        }
        if ((snapshot.left.has_value() && *snapshot.left == 100U) ||
            (snapshot.right.has_value() && *snapshot.right == 100U) ||
            (snapshot.case_level.has_value() && *snapshot.case_level == 100U)) {
            score += 4;
        }

        return Candidate{snapshot, score, presence};
    };

    const std::array<std::uint8_t, 4> little_endian_bytes = {
        static_cast<std::uint8_t>((raw_value >> 0U) & 0xFFU),
        static_cast<std::uint8_t>((raw_value >> 8U) & 0xFFU),
        static_cast<std::uint8_t>((raw_value >> 16U) & 0xFFU),
        static_cast<std::uint8_t>((raw_value >> 24U) & 0xFFU),
    };

    std::optional<Candidate> best;
    auto consider = [&](std::uint8_t left_raw, std::uint8_t right_raw, std::uint8_t case_raw) {
        const auto candidate = evaluate_triplet(left_raw, right_raw, case_raw);
        if (candidate.presence < 2) {
            return;
        }
        if (!best.has_value() || candidate.score > best->score) {
            best = candidate;
        }
    };

    consider(little_endian_bytes[0], little_endian_bytes[1], little_endian_bytes[2]);
    consider(little_endian_bytes[1], little_endian_bytes[2], little_endian_bytes[3]);
    consider(little_endian_bytes[2], little_endian_bytes[1], little_endian_bytes[0]);
    consider(little_endian_bytes[3], little_endian_bytes[2], little_endian_bytes[1]);

    if (best.has_value()) {
        const int threshold = best->presence == 3 ? 70 : 55;
        if (best->score >= threshold) {
            return BuildReadingsFromTriplet(best->snapshot);
        }
    }

    const auto scalar = NormalizeZmiVendorBatteryScalar(static_cast<int>(raw_value));
    if (scalar.has_value()) {
        return {BatteryReading{"main", *scalar}};
    }

    return {};
}

std::vector<BatteryReading> ReadZmiVendorBatteryHintFromPnpAddress(std::uint64_t address) {
    static std::unordered_map<std::uint64_t, std::vector<BatteryReading>> cache;
    if (const auto cached = cache.find(address); cached != cache.end()) {
        return cached->second;
    }

    const auto instance_ids = FindBthEnumInstanceIdsByAddress(address);
    if (instance_ids.empty()) {
        DebugLog("ZMI PnP lookup: instance id not found for address=" + std::to_string(address));
        cache[address] = {};
        return {};
    }

    std::wstringstream prefix_builder;
    prefix_builder << L"BTHENUM\\DEV_" << std::uppercase << std::hex
                   << std::setw(12) << std::setfill(L'0') << address;
    const std::wstring preferred_prefix = prefix_builder.str();

    std::vector<std::wstring> ranked_instance_ids = instance_ids;
    std::stable_sort(ranked_instance_ids.begin(), ranked_instance_ids.end(),
                     [&preferred_prefix](const std::wstring& lhs, const std::wstring& rhs) {
                         auto score = [&preferred_prefix](const std::wstring& value) {
                             int rank = 0;
                             if (value.rfind(preferred_prefix, 0) == 0) {
                                 rank += 100;
                             }
                             if (value.find(L"\\DEV_") != std::wstring::npos) {
                                 rank += 20;
                             }
                             if (value.find(L"{") != std::wstring::npos) {
                                 rank -= 12;
                             }
                             if (value.find(L"LOCALMFG") != std::wstring::npos) {
                                 rank -= 8;
                             }
                             return rank;
                         };
                         return score(lhs) > score(rhs);
                     });

    for (const auto& instance_id : ranked_instance_ids) {
        DEVINST dev_inst = 0;
        const auto locate_result =
            CM_Locate_DevNodeW(&dev_inst, const_cast<wchar_t*>(instance_id.c_str()), CM_LOCATE_DEVNODE_NORMAL);
        if (locate_result != CR_SUCCESS) {
            continue;
        }

        auto decode_property = [&](const DEVPROPKEY& property_key) -> std::vector<BatteryReading> {
            DEVPROPTYPE property_type = DEVPROP_TYPE_EMPTY;
            ULONG size = 0;
            const auto query_result =
                CM_Get_DevNode_PropertyW(dev_inst, &property_key, &property_type, nullptr, &size, 0);
            if (query_result != CR_SUCCESS && query_result != CR_BUFFER_SMALL) {
                return {};
            }
            if (size == 0U) {
                return {};
            }

            std::vector<std::uint8_t> raw_data(size);
            const auto property_result = CM_Get_DevNode_PropertyW(
                dev_inst, &property_key, &property_type,
                reinterpret_cast<PBYTE>(raw_data.data()), &size, 0);
            if (property_result != CR_SUCCESS) {
                return {};
            }
            raw_data.resize(size);
            if (DebugEnabled() && (property_key.pid != 4U || raw_data.size() > 1U)) {
                const std::size_t preview_len = std::min<std::size_t>(raw_data.size(), 24U);
                std::vector<std::uint8_t> preview(raw_data.begin(),
                                                  raw_data.begin() + static_cast<std::ptrdiff_t>(preview_len));
                DebugLog("ZMI PnP raw pid=" + std::to_string(property_key.pid) +
                         " type=" + std::to_string(property_type) +
                         " size=" + std::to_string(raw_data.size()) +
                         " data=" + BytesToHex(preview));
            }

            if (property_type == DEVPROP_TYPE_BYTE && !raw_data.empty()) {
                const auto normalized = NormalizeZmiVendorBatteryScalar(static_cast<int>(raw_data[0]));
                if (normalized.has_value()) {
                    return {BatteryReading{"main", *normalized}};
                }
            }

            if (property_type == DEVPROP_TYPE_UINT32 && size >= sizeof(std::uint32_t)) {
                std::uint32_t value = 0;
                std::memcpy(&value, raw_data.data(), sizeof(value));
                auto decoded = DecodeZmiVendorBatteryReadingsFromUInt32(value);
                if (!decoded.empty()) {
                    return decoded;
                }
            }
            if (property_type == DEVPROP_TYPE_INT32 && size >= sizeof(std::int32_t)) {
                std::int32_t value = 0;
                std::memcpy(&value, raw_data.data(), sizeof(value));
                if (value > 0) {
                    auto decoded = DecodeZmiVendorBatteryReadingsFromUInt32(static_cast<std::uint32_t>(value));
                    if (!decoded.empty()) {
                        return decoded;
                    }
                }
            }

            return DecodeZmiVendorReadingsFromByteBlob(raw_data);
        };

        std::vector<DEVPROPKEY> keys_to_probe;
        constexpr int kVendorPidMin = 1;
        constexpr int kVendorPidMax = 32;
        for (int pid = kVendorPidMin; pid <= kVendorPidMax; ++pid) {
            DEVPROPKEY key = kZmiVendorBatteryHintPropKey;
            key.pid = static_cast<ULONG>(pid);
            keys_to_probe.push_back(key);
        }

        ULONG key_count = 0;
        const auto key_count_result = CM_Get_DevNode_Property_Keys(dev_inst, nullptr, &key_count, 0);
        if ((key_count_result == CR_SUCCESS || key_count_result == CR_BUFFER_SMALL) && key_count > 0U) {
            std::vector<DEVPROPKEY> property_keys(key_count);
            if (CM_Get_DevNode_Property_Keys(dev_inst, property_keys.data(), &key_count, 0) == CR_SUCCESS) {
                property_keys.resize(key_count);
                for (const auto& property_key : property_keys) {
                    if (std::memcmp(&property_key.fmtid, &kZmiVendorBatteryHintPropKey.fmtid, sizeof(GUID)) != 0) {
                        continue;
                    }
                    const bool already_added = std::any_of(
                        keys_to_probe.begin(), keys_to_probe.end(),
                        [&property_key](const DEVPROPKEY& existing) {
                            return std::memcmp(&existing, &property_key, sizeof(DEVPROPKEY)) == 0;
                        });
                    if (!already_added) {
                        keys_to_probe.push_back(property_key);
                    }
                }
            }
        }

        std::sort(keys_to_probe.begin(), keys_to_probe.end(),
                  [](const DEVPROPKEY& lhs, const DEVPROPKEY& rhs) {
                      return lhs.pid < rhs.pid;
                  });
        keys_to_probe.erase(
            std::unique(keys_to_probe.begin(), keys_to_probe.end(),
                        [](const DEVPROPKEY& lhs, const DEVPROPKEY& rhs) {
                            return std::memcmp(&lhs, &rhs, sizeof(DEVPROPKEY)) == 0;
                        }),
            keys_to_probe.end());
        if (DebugEnabled() && keys_to_probe.size() > 1U) {
            std::string pid_list;
            for (const auto& property_key : keys_to_probe) {
                if (!pid_list.empty()) {
                    pid_list += ",";
                }
                pid_list += std::to_string(property_key.pid);
            }
            DebugLog("ZMI PnP lookup: probing vendor pids=" + pid_list +
                     " instance='" + ToUtf8(winrt::hstring(instance_id)) + "'");
        }

        std::vector<BatteryReading> best_single_value;
        for (const auto& property_key : keys_to_probe) {
            auto decoded = decode_property(property_key);
            if (decoded.empty()) {
                continue;
            }

            if (decoded.size() >= 2U) {
                DebugLog("ZMI PnP lookup: vendor property pid=" + std::to_string(property_key.pid) +
                         " decoded entries=" + std::to_string(decoded.size()));
                cache[address] = decoded;
                return decoded;
            }

            if (DebugEnabled()) {
                DebugLog("ZMI PnP lookup: vendor property pid=" + std::to_string(property_key.pid) +
                         " decoded scalar entries=" + std::to_string(decoded.size()));
            }
            if (best_single_value.empty()) {
                best_single_value = decoded;
            }
        }

        if (!best_single_value.empty()) {
            cache[address] = best_single_value;
            return best_single_value;
        }
    }

    DebugLog("ZMI PnP lookup: vendor property missing for address=" + std::to_string(address));
    cache[address] = {};
    return {};
}

std::optional<std::uint64_t> TryGetBluetoothAddress(const BluetoothLEDevice& ble_device) {
    try {
        const auto address = ble_device.BluetoothAddress();
        if (address > 0xFFFFULL) {
            return address;
        }
    } catch (const winrt::hresult_error&) {
    }

    return std::nullopt;
}

bool IsXiaomiRequestType(XiaomiMessageType type) {
    return (static_cast<std::uint8_t>(type) & 0x40U) != 0U;
}

std::vector<std::uint8_t> EncodeXiaomiMessage(const XiaomiMessage& message) {
    const bool is_request = IsXiaomiRequestType(message.type);
    const std::uint16_t payload_length = static_cast<std::uint16_t>(message.payload.size() + (is_request ? 1U : 2U));

    std::vector<std::uint8_t> bytes;
    bytes.reserve(8U + payload_length);
    bytes.insert(bytes.end(), kXiaomiMessageHeader.begin(), kXiaomiMessageHeader.end());
    bytes.push_back(static_cast<std::uint8_t>(message.type));
    bytes.push_back(static_cast<std::uint8_t>(message.opcode));
    bytes.push_back(static_cast<std::uint8_t>((payload_length >> 8) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>(payload_length & 0xFFU));
    if (!is_request) {
        bytes.push_back(0x00);
    }
    bytes.push_back(message.sequence);
    bytes.insert(bytes.end(), message.payload.begin(), message.payload.end());
    bytes.push_back(kXiaomiMessageTrailer);
    return bytes;
}

bool ParseXiaomiMessage(const std::vector<std::uint8_t>& bytes, XiaomiMessage* message) {
    if (message == nullptr || bytes.size() < 9) {
        return false;
    }

    if (!(bytes[0] == kXiaomiMessageHeader[0] && bytes[1] == kXiaomiMessageHeader[1] &&
          bytes[2] == kXiaomiMessageHeader[2])) {
        return false;
    }
    if (bytes.back() != kXiaomiMessageTrailer) {
        return false;
    }

    const auto type = static_cast<XiaomiMessageType>(bytes[3]);
    const bool is_request = IsXiaomiRequestType(type);
    const std::uint16_t payload_length = static_cast<std::uint16_t>((bytes[5] << 8) | bytes[6]);
    const std::size_t expected_size = 8U + payload_length;
    if (bytes.size() != expected_size) {
        return false;
    }

    const std::size_t sequence_offset = is_request ? 7U : 8U;
    const std::size_t payload_offset = sequence_offset + 1U;
    const std::size_t overhead = is_request ? 1U : 2U;
    if (payload_length < overhead) {
        return false;
    }
    const std::size_t actual_payload_length = payload_length - overhead;
    if (payload_offset + actual_payload_length + 1U != bytes.size()) {
        return false;
    }

    message->type = type;
    message->opcode = static_cast<XiaomiOpcode>(bytes[4]);
    message->sequence = bytes[sequence_offset];
    message->payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset),
                            bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset + actual_payload_length));
    return true;
}

std::vector<XiaomiMessage> DecodeXiaomiMessages(std::vector<std::uint8_t>* buffer) {
    std::vector<XiaomiMessage> messages;
    if (buffer == nullptr) {
        return messages;
    }

    std::size_t cursor = 0;
    while (cursor + 8U <= buffer->size()) {
        if (!((*buffer)[cursor] == kXiaomiMessageHeader[0] && (*buffer)[cursor + 1U] == kXiaomiMessageHeader[1] &&
              (*buffer)[cursor + 2U] == kXiaomiMessageHeader[2])) {
            ++cursor;
            continue;
        }

        if (cursor + 7U >= buffer->size()) {
            break;
        }

        const std::uint16_t payload_length =
            static_cast<std::uint16_t>(((*buffer)[cursor + 5U] << 8) | (*buffer)[cursor + 6U]);
        const std::size_t total_length = 8U + payload_length;
        if (cursor + total_length > buffer->size()) {
            break;
        }

        std::vector<std::uint8_t> chunk(buffer->begin() + static_cast<std::ptrdiff_t>(cursor),
                                        buffer->begin() + static_cast<std::ptrdiff_t>(cursor + total_length));
        XiaomiMessage parsed;
        if (ParseXiaomiMessage(chunk, &parsed)) {
            messages.push_back(std::move(parsed));
        }
        cursor += total_length;
    }

    if (cursor > 0) {
        buffer->erase(buffer->begin(), buffer->begin() + static_cast<std::ptrdiff_t>(cursor));
    }

    return messages;
}

std::optional<std::uint8_t> ParseXiaomiNoiseModeCodeFromRunInfoPayload(const std::vector<std::uint8_t>& payload) {
    for (std::size_t index = 0; index + 2U < payload.size();) {
        const std::size_t len = payload[index];
        const std::size_t field_size = len + 1U;
        if (len < 2U || index + field_size > payload.size()) {
            break;
        }
        const std::uint8_t tag = payload[index + 1U];
        if (tag == 0x09U && len >= 2U) {
            return payload[index + 2U];
        }
        index += field_size;
    }
    return std::nullopt;
}

std::optional<std::uint8_t> ParseXiaomiNoiseModeCodeFromStatusPayload(const std::vector<std::uint8_t>& payload) {
    if (payload.size() >= 3U && payload[0] == 0x02U && payload[1] == 0x04U) {
        return payload[2];
    }
    return std::nullopt;
}

std::optional<std::uint8_t> ParseXiaomiNoiseModeCodeFromF4Payload(const std::vector<std::uint8_t>& payload) {
    if (payload.size() >= 4U && payload[0] == 0x04U && payload[1] == 0x00U && payload[2] == 0x0BU) {
        return payload[3];
    }
    return std::nullopt;
}

std::optional<std::uint8_t> ParseXiaomiNoiseSubmodeCodeFromF4Payload(const std::vector<std::uint8_t>& payload) {
    if (payload.size() >= 5U && payload[0] == 0x04U && payload[1] == 0x00U && payload[2] == 0x0BU) {
        return payload[4];
    }
    return std::nullopt;
}

std::optional<std::uint8_t> ParseXiaomiNoiseModeCode(std::uint8_t opcode, const std::vector<std::uint8_t>& payload) {
    if (opcode == static_cast<std::uint8_t>(XiaomiOpcode::kGetDeviceRunInfo)) {
        return ParseXiaomiNoiseModeCodeFromRunInfoPayload(payload);
    }
    if (opcode == static_cast<std::uint8_t>(XiaomiOpcode::kReportStatus)) {
        return ParseXiaomiNoiseModeCodeFromStatusPayload(payload);
    }
    if (opcode == 0xF4U) {
        return ParseXiaomiNoiseModeCodeFromF4Payload(payload);
    }
    return std::nullopt;
}

std::string XiaomiNoiseModeCodeToText(std::uint8_t code) {
    if (code == 0U) {
        return "off";
    }
    if (code == 2U) {
        return "transparency";
    }
    if (code == 1U) {
        return "anc";
    }
    return "mode " + std::to_string(code);
}

std::optional<std::string> XiaomiNoiseSubmodeCodeToText(std::uint8_t mode_code, std::uint8_t submode_code) {
    if (mode_code == 1U) {
        if (submode_code == 0U) {
            return std::string("balanced");
        }
        if (submode_code == 1U) {
            return std::string("weak");
        }
        if (submode_code == 2U) {
            return std::string("deep");
        }
        if (submode_code == 3U) {
            return std::string("adaptive");
        }
    }
    if (mode_code == 2U) {
        if (submode_code == 0U) {
            return std::string("standard");
        }
        if (submode_code == 1U) {
            return std::string("voice");
        }
    }
    return std::nullopt;
}

void PutXiaomiModeCacheEntry(std::uint64_t address,
                             std::uint8_t code,
                             std::optional<std::uint8_t> submode_code = std::nullopt) {
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

std::uint32_t ModPow(std::uint32_t base, std::uint32_t exponent, std::uint32_t modulo) {
    std::uint64_t result = 1;
    std::uint64_t value = base % modulo;
    std::uint32_t exp = exponent;

    while (exp > 0U) {
        if ((exp & 1U) != 0U) {
            result = (result * value) % modulo;
        }
        value = (value * value) % modulo;
        exp >>= 1U;
    }

    return static_cast<std::uint32_t>(result);
}

std::array<std::array<std::uint8_t, 16>, 16> BuildXiaomiBiasMatrix() {
    std::array<std::array<std::uint8_t, 16>, 16> bias{};
    for (std::size_t i = 0; i < 16; ++i) {
        for (std::size_t j = 0; j < 16; ++j) {
            const std::uint32_t exponent = static_cast<std::uint32_t>(17U * (i + 2U) + (j + 1U));
            const std::uint32_t inner = ModPow(45U, exponent, 257U);
            const std::uint32_t outer = ModPow(45U, inner, 257U);
            bias[i][j] = static_cast<std::uint8_t>(outer == 256U ? 0U : outer);
        }
    }
    return bias;
}

std::array<std::uint8_t, 256> BuildXiaomiExpTable() {
    std::array<std::uint8_t, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (i == 128U) {
            table[i] = 0;
            continue;
        }
        table[i] = static_cast<std::uint8_t>(ModPow(45U, static_cast<std::uint32_t>(i), 257U));
    }
    return table;
}

std::array<std::uint8_t, 256> BuildXiaomiLogTable() {
    std::array<std::uint8_t, 256> table{};
    table[0] = 128U;
    for (std::uint32_t i = 1; i < 256U; ++i) {
        const std::uint32_t mod_exp = ModPow(45U, i, 257U);
        if (mod_exp != 256U) {
            table[mod_exp] = static_cast<std::uint8_t>(i);
        }
    }
    return table;
}

std::array<std::array<std::uint8_t, 16>, 17> BuildXiaomiKeySchedule(std::array<std::uint8_t, 16> key_init) {
    static const auto bias_matrix = BuildXiaomiBiasMatrix();

    key_init[15] ^= 0x06U;

    std::array<std::array<std::uint8_t, 16>, 17> keys{};
    keys[0] = key_init;

    std::array<std::uint8_t, 17> reg{};
    std::uint8_t xor_sum = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        reg[i] = key_init[i];
        xor_sum ^= key_init[i];
    }
    reg[16] = xor_sum;

    for (std::size_t key_idx = 1; key_idx < 17; ++key_idx) {
        for (auto& value : reg) {
            value = static_cast<std::uint8_t>(((value & 0xFFU) >> 5U) | ((value & 0xFFU) << 3U));
        }

        std::array<std::uint8_t, 16> key_i{};
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t reg_idx = (key_idx + i) % 17U;
            key_i[i] = static_cast<std::uint8_t>(reg[reg_idx] + bias_matrix[key_idx - 1U][i]);
        }
        keys[key_idx] = key_i;
    }

    return keys;
}

std::array<std::uint8_t, 16> ComputeXiaomiChallengeResponse(const std::array<std::uint8_t, 16>& challenge) {
    static const auto exp_tab = BuildXiaomiExpTable();
    static const auto log_tab = BuildXiaomiLogTable();

    auto keys = BuildXiaomiKeySchedule(challenge);
    std::array<std::uint8_t, 16> ciphertext = kXiaomiAuthSeq;
    const std::array<std::uint8_t, 16> plaintext = kXiaomiAuthSeq;

    for (int round = 0; round < 8; ++round) {
        if (round == 2) {
            for (std::size_t i = 0; i < 16; ++i) {
                if (((1 << static_cast<int>(i)) & kXiaomiAuthPattern) != 0) {
                    ciphertext[i] ^= plaintext[i];
                } else {
                    ciphertext[i] = static_cast<std::uint8_t>(ciphertext[i] + plaintext[i]);
                }
            }
        }

        for (std::size_t i = 0; i < 16; ++i) {
            if (((1 << static_cast<int>(i)) & kXiaomiAuthPattern) != 0) {
                ciphertext[i] ^= keys[round * 2][i];
            } else {
                ciphertext[i] = static_cast<std::uint8_t>(ciphertext[i] + keys[round * 2][i]);
            }
        }

        for (std::size_t i = 0; i < 16; ++i) {
            if (((1 << static_cast<int>(i)) & kXiaomiAuthPattern) != 0) {
                ciphertext[i] = exp_tab[ciphertext[i]];
            } else {
                ciphertext[i] = log_tab[ciphertext[i]];
            }
        }

        for (std::size_t i = 0; i < 16; ++i) {
            if (((1 << static_cast<int>(i)) & kXiaomiAuthPattern) != 0) {
                ciphertext[i] = static_cast<std::uint8_t>(keys[round * 2 + 1][i] + ciphertext[i]);
            } else {
                ciphertext[i] = static_cast<std::uint8_t>(keys[round * 2 + 1][i] ^ ciphertext[i]);
            }
        }

        const auto copy = ciphertext;
        for (std::size_t i = 0; i < 16; ++i) {
            std::uint8_t sum = 0;
            for (std::size_t j = 0; j < 16; ++j) {
                const int product = kXiaomiAuthCoefficients[i][j] * static_cast<int>(copy[j]);
                sum = static_cast<std::uint8_t>(sum + product);
            }
            ciphertext[i] = sum;
        }
    }

    for (std::size_t i = 0; i < 16; ++i) {
        if (((1 << static_cast<int>(i)) & kXiaomiAuthPattern) != 0) {
            ciphertext[i] = static_cast<std::uint8_t>(keys[16][i] ^ ciphertext[i]);
        } else {
            ciphertext[i] = static_cast<std::uint8_t>(keys[16][i] + ciphertext[i]);
        }
    }

    return ciphertext;
}

std::array<std::uint8_t, 16> GenerateRandomChallenge() {
    std::array<std::uint8_t, 16> challenge{};
    std::random_device random;
    for (auto& value : challenge) {
        value = static_cast<std::uint8_t>(random() & 0xFFU);
    }
    return challenge;
}

bool SendAll(SOCKET socket_handle, const std::vector<std::uint8_t>& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const int chunk = send(socket_handle, reinterpret_cast<const char*>(bytes.data() + sent),
                               static_cast<int>(bytes.size() - sent), 0);
        if (chunk <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(chunk);
    }
    return true;
}

bool ConnectWithTimeout(SOCKET socket_handle, const SOCKADDR_BTH& address, int timeout_ms) {
    u_long non_blocking = 1;
    if (ioctlsocket(socket_handle, FIONBIO, &non_blocking) == SOCKET_ERROR) {
        return false;
    }

    const int connect_result =
        connect(socket_handle, reinterpret_cast<const SOCKADDR*>(&address), sizeof(address));
    if (connect_result == SOCKET_ERROR) {
        const int connect_error = WSAGetLastError();
        if (connect_error != WSAEWOULDBLOCK && connect_error != WSAEINPROGRESS && connect_error != WSAEINVAL) {
            non_blocking = 0;
            ioctlsocket(socket_handle, FIONBIO, &non_blocking);
            WSASetLastError(connect_error);
            return false;
        }

        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(socket_handle, &write_set);

        TIMEVAL timeout{};
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;

        const int select_result = select(0, nullptr, &write_set, nullptr, &timeout);
        if (select_result <= 0) {
            non_blocking = 0;
            ioctlsocket(socket_handle, FIONBIO, &non_blocking);
            WSASetLastError(WSAETIMEDOUT);
            return false;
        }

        int socket_error = 0;
        int option_length = sizeof(socket_error);
        if (getsockopt(socket_handle, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&socket_error), &option_length) == SOCKET_ERROR ||
            socket_error != 0) {
            non_blocking = 0;
            ioctlsocket(socket_handle, FIONBIO, &non_blocking);
            WSASetLastError(socket_error == 0 ? WSAECONNABORTED : socket_error);
            return false;
        }
    }

    non_blocking = 0;
    ioctlsocket(socket_handle, FIONBIO, &non_blocking);
    return true;
}

struct DiscoveredRfcommChannel {
    std::uint32_t port = 0;
    GUID service_uuid{};
    std::string instance_name;
};

std::optional<std::wstring> BuildBluetoothLookupContext(std::uint64_t bluetooth_address) {
    SOCKADDR_BTH context_address{};
    context_address.addressFamily = AF_BTH;
    context_address.btAddr = bluetooth_address;
    context_address.port = BT_PORT_ANY;
    context_address.serviceClassId = GUID{};

    std::array<wchar_t, 96> context_text{};
    DWORD context_length = static_cast<DWORD>(context_text.size());
    if (WSAAddressToStringW(reinterpret_cast<LPSOCKADDR>(&context_address), sizeof(context_address),
                            nullptr, context_text.data(), &context_length) == SOCKET_ERROR) {
        return std::nullopt;
    }

    return std::wstring(context_text.data());
}

std::vector<DiscoveredRfcommChannel> DiscoverRfcommChannelsFromSdp(std::uint64_t bluetooth_address,
                                                                   const GUID* service_filter,
                                                                   bool flush_cache) {
    std::vector<DiscoveredRfcommChannel> channels;
    const auto lookup_context = BuildBluetoothLookupContext(bluetooth_address);
    if (!lookup_context.has_value()) {
        return channels;
    }

    WSAQUERYSETW query{};
    query.dwSize = sizeof(query);
    query.dwNameSpace = NS_BTH;
    query.lpszContext = const_cast<LPWSTR>(lookup_context->c_str());
    if (service_filter != nullptr) {
        query.lpServiceClassId = const_cast<LPGUID>(service_filter);
    }

    HANDLE lookup_handle = nullptr;
    DWORD begin_flags = LUP_RETURN_ADDR | LUP_RETURN_NAME;
    if (flush_cache) {
        begin_flags |= LUP_FLUSHCACHE;
    }

    if (WSALookupServiceBeginW(&query, begin_flags, &lookup_handle) == SOCKET_ERROR) {
        return channels;
    }

    std::set<std::uint32_t> seen_ports;
    std::vector<std::uint8_t> buffer(4096);

    while (true) {
        DWORD buffer_length = static_cast<DWORD>(buffer.size());
        auto* result = reinterpret_cast<WSAQUERYSETW*>(buffer.data());
        std::memset(result, 0, buffer.size());
        result->dwSize = sizeof(WSAQUERYSETW);

        if (WSALookupServiceNextW(lookup_handle, LUP_RETURN_ADDR | LUP_RETURN_NAME,
                                  &buffer_length, result) == SOCKET_ERROR) {
            const int error_code = WSAGetLastError();
            if (error_code == WSAEFAULT && buffer_length > buffer.size()) {
                buffer.resize(buffer_length);
                continue;
            }
            break;
        }

        if (result->lpcsaBuffer == nullptr || result->dwNumberOfCsAddrs == 0U) {
            continue;
        }

        for (DWORD index = 0; index < result->dwNumberOfCsAddrs; ++index) {
            const auto& csaddr = result->lpcsaBuffer[index];
            if (csaddr.RemoteAddr.lpSockaddr == nullptr ||
                csaddr.RemoteAddr.iSockaddrLength < static_cast<int>(sizeof(SOCKADDR_BTH))) {
                continue;
            }

            const auto* remote = reinterpret_cast<const SOCKADDR_BTH*>(csaddr.RemoteAddr.lpSockaddr);
            if (remote->addressFamily != AF_BTH) {
                continue;
            }
            if (remote->port == BT_PORT_ANY || remote->port == 0U || remote->port > 60U) {
                continue;
            }
            if (!seen_ports.insert(remote->port).second) {
                continue;
            }

            DiscoveredRfcommChannel channel;
            channel.port = remote->port;
            channel.service_uuid = service_filter != nullptr ? *service_filter : remote->serviceClassId;
            if (result->lpszServiceInstanceName != nullptr) {
                channel.instance_name = winrt::to_string(winrt::hstring(result->lpszServiceInstanceName));
            }
            channels.push_back(std::move(channel));
        }
    }

    WSALookupServiceEnd(lookup_handle);
    return channels;
}

std::optional<std::vector<std::uint8_t>> ReceiveChunk(SOCKET socket_handle) {
    std::array<char, 4096> recv_buffer{};
    const int received = recv(socket_handle, recv_buffer.data(), static_cast<int>(recv_buffer.size()), 0);
    if (received <= 0) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(received));
    std::memcpy(bytes.data(), recv_buffer.data(), static_cast<std::size_t>(received));
    return bytes;
}

std::optional<std::uint8_t> ParseXiaomiBatteryRaw(std::uint8_t raw_value) {
    if (raw_value == 0xFFU) {
        return std::nullopt;
    }
    const auto level = static_cast<std::uint8_t>(raw_value & 0x7FU);
    if (level > 100U) {
        return std::nullopt;
    }
    return level;
}

int XiaomiBatteryPresenceCount(const XiaomiBatterySnapshot& snapshot) {
    int count = 0;
    if (snapshot.left.has_value()) {
        ++count;
    }
    if (snapshot.right.has_value()) {
        ++count;
    }
    if (snapshot.case_level.has_value()) {
        ++count;
    }
    return count;
}

std::optional<XiaomiBatterySnapshot> ExtractBatterySnapshotFromXiaomiPayload(
    const std::vector<std::uint8_t>& payload, std::optional<std::uint8_t> preferred_tag) {
    struct Candidate {
        XiaomiBatterySnapshot snapshot;
        std::uint8_t tag = 0;
        int score = 0;
    };

    std::optional<Candidate> best_candidate;

    std::size_t index = 0;
    while (index + 1U < payload.size()) {
        const std::size_t len = payload[index];
        if (len == 0U || index + len >= payload.size()) {
            ++index;
            continue;
        }

        const std::uint8_t tag = payload[index + 1U];
        if (len < 4U || len > 8U || tag > 0x7FU) {
            index += len + 1U;
            continue;
        }

        const std::uint8_t left_raw = payload[index + 2U];
        const std::uint8_t right_raw = payload[index + 3U];
        const std::uint8_t case_raw = payload[index + 4U];

        XiaomiBatterySnapshot snapshot;
        snapshot.left = ParseXiaomiBatteryRaw(left_raw);
        snapshot.right = ParseXiaomiBatteryRaw(right_raw);
        snapshot.case_level = ParseXiaomiBatteryRaw(case_raw);

        if (!snapshot.left.has_value() && !snapshot.right.has_value() && !snapshot.case_level.has_value()) {
            index += len + 1U;
            continue;
        }

        int score = 0;
        if (preferred_tag.has_value() && tag == *preferred_tag) {
            score += 50;
        }
        if (!preferred_tag.has_value() &&
            (tag == 0x00U || tag == 0x07U || tag == 0x09U || tag == 0x0AU)) {
            score += 12;
        }

        score += 15;

        const int presence = XiaomiBatteryPresenceCount(snapshot);
        score += presence * 10;
        if (snapshot.left.has_value() && snapshot.right.has_value() && *snapshot.left != *snapshot.right) {
            score += 4;
        }
        if ((snapshot.left.has_value() && *snapshot.left == 100U) ||
            (snapshot.right.has_value() && *snapshot.right == 100U) ||
            (snapshot.case_level.has_value() && *snapshot.case_level == 100U)) {
            score += 2;
        }

        if (DebugEnabled()) {
            const std::string left_text = snapshot.left.has_value() ? std::to_string(*snapshot.left) : "na";
            const std::string right_text = snapshot.right.has_value() ? std::to_string(*snapshot.right) : "na";
            const std::string case_text = snapshot.case_level.has_value() ? std::to_string(*snapshot.case_level) : "na";
            DebugLog("Xiaomi payload battery candidate tag=" + std::to_string(tag) +
                     " raw=(" + std::to_string(left_raw) + "," + std::to_string(right_raw) + "," +
                     std::to_string(case_raw) + ")" +
                     " level=(" + left_text + "," + right_text + "," + case_text + ")" +
                     " score=" + std::to_string(score));
        }

        if (!best_candidate.has_value() || score > best_candidate->score) {
            best_candidate = Candidate{snapshot, tag, score};
        }

        if (preferred_tag.has_value() && tag == *preferred_tag && score >= 70) {
            return snapshot;
        }

        index += len + 1U;
    }

    if (best_candidate.has_value()) {
        return best_candidate->snapshot;
    }

    return std::nullopt;
}

std::optional<XiaomiBatterySnapshot> ExtractPreferredXiaomiBatterySnapshot(
    const std::vector<std::uint8_t>& payload) {
    if (const auto preferred_device_info =
            ExtractBatterySnapshotFromXiaomiPayload(
                payload, std::optional<std::uint8_t>{static_cast<std::uint8_t>(0x07U)});
        preferred_device_info.has_value()) {
        return preferred_device_info;
    }

    if (const auto preferred_status =
            ExtractBatterySnapshotFromXiaomiPayload(
                payload, std::optional<std::uint8_t>{static_cast<std::uint8_t>(0x00U)});
        preferred_status.has_value()) {
        return preferred_status;
    }

    return ExtractBatterySnapshotFromXiaomiPayload(payload, std::nullopt);
}

XiaomiBatterySnapshot MergeXiaomiSnapshots(const XiaomiBatterySnapshot& preferred, const XiaomiBatterySnapshot& fallback) {
    XiaomiBatterySnapshot merged = preferred;
    if (!merged.left.has_value()) {
        merged.left = fallback.left;
    }
    if (!merged.right.has_value()) {
        merged.right = fallback.right;
    }
    if (!merged.case_level.has_value()) {
        merged.case_level = fallback.case_level;
    }
    return merged;
}

std::vector<BatteryReading> BuildXiaomiBatteryReadings(const XiaomiBatterySnapshot& snapshot) {
    std::vector<BatteryReading> readings;
    if (snapshot.left.has_value()) {
        readings.push_back(BatteryReading{"left", *snapshot.left});
    }
    if (snapshot.right.has_value()) {
        readings.push_back(BatteryReading{"right", *snapshot.right});
    }
    if (snapshot.case_level.has_value()) {
        readings.push_back(BatteryReading{"case", *snapshot.case_level});
    }
    return readings;
}

std::optional<XiaomiBatterySnapshot> ExtractZmiSerialPatternSnapshot(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 7U) {
        return std::nullopt;
    }

    constexpr std::array<std::uint8_t, 4> kPattern = {0x02, 0x02, 0x04, 0x07};
    for (std::size_t index = 0; index + 6U < bytes.size(); ++index) {
        if (!(bytes[index] == kPattern[0] &&
              bytes[index + 1U] == kPattern[1] &&
              bytes[index + 2U] == kPattern[2] &&
              bytes[index + 3U] == kPattern[3])) {
            continue;
        }

        XiaomiBatterySnapshot snapshot;
        snapshot.left = ParseXiaomiBatteryRaw(bytes[index + 4U]);
        snapshot.right = ParseXiaomiBatteryRaw(bytes[index + 5U]);
        snapshot.case_level = ParseXiaomiBatteryRaw(bytes[index + 6U]);
        if (XiaomiBatteryPresenceCount(snapshot) >= 2) {
            return snapshot;
        }
    }

    return std::nullopt;
}

std::optional<XiaomiBatterySnapshot> ExtractZmiSerialTextSnapshot(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }

    const std::string lowered = ToLowerAscii(text);
    const bool has_component_hints =
        lowered.find("left") != std::string::npos ||
        lowered.find("right") != std::string::npos ||
        lowered.find("case") != std::string::npos ||
        lowered.find(" l:") != std::string::npos ||
        lowered.find(" r:") != std::string::npos ||
        lowered.find(" c:") != std::string::npos ||
        lowered.find("l=") != std::string::npos ||
        lowered.find("r=") != std::string::npos ||
        lowered.find("c=") != std::string::npos;
    const bool has_triplet_probe_hint =
        lowered.find("+xevent") != std::string::npos ||
        lowered.find("xevent:") != std::string::npos ||
        lowered.find("+batt:") != std::string::npos ||
        lowered.find("battery:") != std::string::npos ||
        lowered.find("battery,") != std::string::npos;
    if (!has_component_hints && !has_triplet_probe_hint) {
        return std::nullopt;
    }

    std::vector<int> numbers;
    int current = -1;
    for (const char ch : lowered) {
        if (ch >= '0' && ch <= '9') {
            if (current < 0) {
                current = 0;
            }
            current = (current * 10) + static_cast<int>(ch - '0');
            continue;
        }
        if (current >= 0) {
            numbers.push_back(current);
            current = -1;
        }
    }
    if (current >= 0) {
        numbers.push_back(current);
    }

    if (numbers.size() < 3U) {
        return std::nullopt;
    }

    XiaomiBatterySnapshot best_snapshot;
    int best_presence = -1;

    for (std::size_t index = 0; index + 2U < numbers.size(); ++index) {
        XiaomiBatterySnapshot snapshot;
        snapshot.left = NormalizeZmiVendorBatteryScalar(numbers[index]);
        snapshot.right = NormalizeZmiVendorBatteryScalar(numbers[index + 1U]);
        snapshot.case_level = NormalizeZmiVendorBatteryScalar(numbers[index + 2U]);

        const int presence = XiaomiBatteryPresenceCount(snapshot);
        if (presence > best_presence) {
            best_presence = presence;
            best_snapshot = snapshot;
        }
    }

    if (best_presence >= 2) {
        return best_snapshot;
    }

    return std::nullopt;
}

void ReplyToHfpAgCommand(SOCKET socket_handle, const std::string& line);
std::optional<std::uint8_t> ParseAtBatteryPercentFromLine(const std::string& line);
std::optional<std::uint8_t> NormalizeCindBatteryPercent(int value);

std::vector<BatteryReading> TryReadZmiSerialBatteryFromSocket(SOCKET socket_handle) {
    std::vector<BatteryReading> readings;
    if (socket_handle == INVALID_SOCKET) {
        return readings;
    }

    const int recv_timeout_ms = 75;
    const int send_timeout_ms = 120;
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&recv_timeout_ms), sizeof(recv_timeout_ms));
    setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&send_timeout_ms), sizeof(send_timeout_ms));

    auto make_probe_message =
        [](XiaomiMessageType type,
           XiaomiOpcode opcode,
           std::uint8_t sequence,
           std::initializer_list<std::uint8_t> payload) {
            XiaomiMessage message;
            message.type = type;
            message.opcode = opcode;
            message.sequence = sequence;
            message.payload.assign(payload.begin(), payload.end());
            return EncodeXiaomiMessage(message);
        };

    std::vector<std::vector<std::uint8_t>> probes;
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0x02, 0x00, 0x05, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x42});
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0x09, 0x00, 0x05, 0x01,
        0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x42});
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0x02, 0x00, 0x05, 0x02,
        0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x42});
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0x09, 0x00, 0x05, 0x08,
        0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x42});
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0xF1, 0x00, 0x07, 0x03,
        0x5A, 0x4D, 0xEA, 0x02, 0x81, 0x00, 0xEF, 0x5E});
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0xF1, 0x00, 0x07, 0x04,
        0x5A, 0x4D, 0xEA, 0x02, 0x81, 0x00, 0xEF, 0x5E});
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0xF1, 0x00, 0x07, 0x05,
        0x5A, 0x4D, 0xEA, 0x02, 0x81, 0x00, 0xEF, 0x5E});

    constexpr std::array<const char*, 33> kTextProbes = {
        "AT\r",
        "AT+XAPL=ABCD-1234-0100,7\r",
        "AT+CIND?\r",
        "AT+CIND=?\r",
        "AT+BRSF=20\r",
        "AT+IPHONEACCEV?\r",
        "AT+XEVENT?\r",
        "AT+XEVENT=BATTERY?\r",
        "AT+XEVENT=BATTERY\r",
        "AT+XEVENT=BATTERY,?\r",
        "AT+XEVENT=BATTERY,GET\r",
        "AT+XEVENT=BATTERY,0\r",
        "AT+XEVENT=BATTERY,1\r",
        "AT+XEVENT=GETBATTERY\r",
        "AT+XEVENT=GETBAT\r",
        "AT+XEVENT=STATUS?\r",
        "AT+BIEV?\r",
        "AT+CMER=3,0,0,1\r",
        "AT+BATT?\r",
        "AT+BATTERY?\r",
        "AT+QBAT?\r",
        "AT+BLEGETBAT?\r",
        "AT+GETBAT?\r",
        "AT+MBATT?\r",
        "AT+MIBAT?\r",
        "AT+BAT?\r",
        "AT+EARBAT?\r",
        "AT+PODBAT?\r",
        "AT+XIAOMI?\r",
        "AT+XMINFO?\r",
        "AT+STATUS?\r",
        "AT+VGS?\r",
        "AT+VGM?\r",
    };

    std::vector<std::uint8_t> rolling_buffer;
    rolling_buffer.reserve(4096);
    std::vector<std::uint8_t> message_buffer;
    message_buffer.reserve(4096);
    std::optional<std::uint8_t> fallback_main;

    auto process_chunk = [&](const std::vector<std::uint8_t>& bytes, const char* source_tag) -> bool {
        if (bytes.empty()) {
            return false;
        }

        if (DebugEnabled()) {
            DebugLog(std::string("ZMI serial rx source=") + source_tag +
                     " len=" + std::to_string(bytes.size()) +
                     " data=" + BytesToHex(bytes));
        }

        rolling_buffer.insert(rolling_buffer.end(), bytes.begin(), bytes.end());
        if (rolling_buffer.size() > 8192U) {
            rolling_buffer.erase(rolling_buffer.begin(),
                                 rolling_buffer.begin() + static_cast<std::ptrdiff_t>(rolling_buffer.size() - 8192U));
        }

        if (const auto snapshot = ExtractZmiSerialPatternSnapshot(bytes); snapshot.has_value()) {
            readings = BuildXiaomiBatteryReadings(*snapshot);
            if (HasUsefulXiaomiTwsReadings(readings)) {
                DebugLog("ZMI serial fallback: extracted triplet from pattern (chunk)");
                return true;
            }
        }
        if (const auto snapshot = ExtractZmiSerialPatternSnapshot(rolling_buffer); snapshot.has_value()) {
            readings = BuildXiaomiBatteryReadings(*snapshot);
            if (HasUsefulXiaomiTwsReadings(readings)) {
                DebugLog("ZMI serial fallback: extracted triplet from pattern (rolling)");
                return true;
            }
        }

        if (const auto snapshot = ExtractPreferredXiaomiBatterySnapshot(bytes); snapshot.has_value()) {
            readings = BuildXiaomiBatteryReadings(*snapshot);
            if (HasUsefulXiaomiTwsReadings(readings)) {
                DebugLog("ZMI serial fallback: extracted triplet from raw payload");
                return true;
            }
        }

        message_buffer.insert(message_buffer.end(), bytes.begin(), bytes.end());
        if (message_buffer.size() > 16384U) {
            message_buffer.erase(message_buffer.begin(),
                                 message_buffer.begin() + static_cast<std::ptrdiff_t>(message_buffer.size() - 16384U));
        }
        const auto messages = DecodeXiaomiMessages(&message_buffer);
        for (const auto& message : messages) {
            if (DebugEnabled()) {
                DebugLog("ZMI serial message type=" + ByteToHex(static_cast<std::uint8_t>(message.type)) +
                         " opcode=" + ByteToHex(static_cast<std::uint8_t>(message.opcode)) +
                         " seq=" + std::to_string(message.sequence) +
                         " payload=" + BytesToHex(message.payload));
            }

            if (message.payload.empty()) {
                continue;
            }

            auto extracted = ExtractPreferredXiaomiBatterySnapshot(message.payload);
            if (extracted.has_value()) {
                const auto candidate = BuildXiaomiBatteryReadings(*extracted);
                if (XiaomiReadingsRichnessScore(candidate) > XiaomiReadingsRichnessScore(readings)) {
                    readings = candidate;
                }
                if (HasUsefulXiaomiTwsReadings(readings)) {
                    DebugLog("ZMI serial fallback: extracted triplet from Xiaomi message payload");
                    return true;
                }
            }
        }

        const std::string chunk_text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::size_t line_start = 0;
        while (line_start < chunk_text.size()) {
            const auto line_end = chunk_text.find_first_of("\r\n", line_start);
            std::string line;
            if (line_end == std::string::npos) {
                line = chunk_text.substr(line_start);
                line_start = chunk_text.size();
            } else {
                line = chunk_text.substr(line_start, line_end - line_start);
                line_start = line_end + 1U;
            }
            if (line.empty()) {
                continue;
            }

            ReplyToHfpAgCommand(socket_handle, line);
            if (const auto parsed = ParseAtBatteryPercentFromLine(line); parsed.has_value()) {
                fallback_main = parsed;
            }
            if (const auto snapshot = ExtractZmiSerialTextSnapshot(line); snapshot.has_value()) {
                const auto candidate = BuildXiaomiBatteryReadings(*snapshot);
                if (XiaomiReadingsRichnessScore(candidate) > XiaomiReadingsRichnessScore(readings)) {
                    readings = candidate;
                }
                if (HasUsefulXiaomiTwsReadings(readings)) {
                    DebugLog("ZMI serial fallback: extracted triplet from text line");
                    return true;
                }
            }
        }
        if (const auto snapshot = ExtractZmiSerialTextSnapshot(chunk_text); snapshot.has_value()) {
            const auto candidate = BuildXiaomiBatteryReadings(*snapshot);
            if (XiaomiReadingsRichnessScore(candidate) > XiaomiReadingsRichnessScore(readings)) {
                readings = candidate;
            }
            if (HasUsefulXiaomiTwsReadings(readings)) {
                DebugLog("ZMI serial fallback: extracted triplet from text chunk");
                return true;
            }
        }

        return false;
    };

    auto receive_attempts = [&](int attempts, int wait_ms, const char* source_tag) {
        for (int attempt = 0; attempt < attempts; ++attempt) {
            if (wait_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
            }
            const auto chunk = ReceiveChunk(socket_handle);
            if (!chunk.has_value() || chunk->empty()) {
                continue;
            }
            if (process_chunk(*chunk, source_tag)) {
                return true;
            }
        }
        return false;
    };

    if (receive_attempts(2, 25, "warmup")) {
        return readings;
    }

    for (const auto& probe : probes) {
        if (!SendAll(socket_handle, probe)) {
            continue;
        }
        if (receive_attempts(2, 35, "binary-probe")) {
            return readings;
        }
    }

    auto send_text_probe = [&](const char* probe) {
        const std::size_t command_length = std::strlen(probe);
        if (send(socket_handle, probe, static_cast<int>(command_length), 0) <= 0) {
            return false;
        }
        if (command_length > 0U && probe[command_length - 1U] == '\r') {
            std::string probe_crlf(probe, command_length);
            probe_crlf.push_back('\n');
            send(socket_handle, probe_crlf.data(), static_cast<int>(probe_crlf.size()), 0);
        }
        return true;
    };

    for (const auto* probe : kTextProbes) {
        if (!send_text_probe(probe)) {
            continue;
        }
        if (receive_attempts(1, 30, "text-probe")) {
            return readings;
        }
    }

    const int observe_ms = ZmiObserveMs();
    if (observe_ms > 0) {
        const auto observe_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(observe_ms);
        auto next_active_probe_at = std::chrono::steady_clock::now();
        std::size_t next_binary_probe = 0;
        std::size_t next_text_probe = 0;
        while (std::chrono::steady_clock::now() < observe_deadline) {
            const auto chunk = ReceiveChunk(socket_handle);
            if (chunk.has_value() && !chunk->empty()) {
                if (process_chunk(*chunk, "observe")) {
                    return readings;
                }
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= next_active_probe_at) {
                if (!probes.empty()) {
                    const auto& probe = probes[next_binary_probe % probes.size()];
                    ++next_binary_probe;
                    SendAll(socket_handle, probe);
                }
                if (!kTextProbes.empty()) {
                    send_text_probe(kTextProbes[next_text_probe % kTextProbes.size()]);
                    ++next_text_probe;
                }
                next_active_probe_at = now + std::chrono::milliseconds(650);
            }

            if (receive_attempts(1, 20, "observe-probe")) {
                return readings;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(35));
        }
    }

    if (!readings.empty()) {
        return readings;
    }

    if (fallback_main.has_value()) {
        readings.push_back(BatteryReading{"main", *fallback_main});
    }

    return readings;
}

std::vector<int> ExtractIntegersFromText(const std::string& text) {
    std::vector<int> numbers;
    int current = -1;
    for (const char ch : text) {
        if (ch >= '0' && ch <= '9') {
            if (current < 0) {
                current = 0;
            }
            current = (current * 10) + static_cast<int>(ch - '0');
            continue;
        }

        if (current >= 0) {
            numbers.push_back(current);
            current = -1;
        }
    }

    if (current >= 0) {
        numbers.push_back(current);
    }

    return numbers;
}

std::optional<std::uint8_t> NormalizeAtBatteryPercent(int value) {
    if (value < 0) {
        return std::nullopt;
    }
    if (value <= 9) {
        return static_cast<std::uint8_t>(std::min(100, value * 10));
    }
    if (value <= 100) {
        return static_cast<std::uint8_t>(value);
    }
    return std::nullopt;
}

std::optional<std::uint8_t> ParseAtBatteryPercentFromLine(const std::string& line) {
    const std::string lowered = ToLowerAscii(line);
    const auto numbers = ExtractIntegersFromText(lowered);
    if (numbers.empty()) {
        return std::nullopt;
    }

    if (lowered.find("+iphoneaccev") != std::string::npos) {
        if (numbers.size() >= 2U) {
            return NormalizeAtBatteryPercent(numbers.back());
        }
        return std::nullopt;
    }

    // +CIND capability declaration includes ranges like ("battchg",(0-5)); it is not a current battery value.
    if (lowered.find("+cind:") != std::string::npos) {
        if (lowered.find('\"') != std::string::npos ||
            lowered.find('(') != std::string::npos ||
            lowered.find('-') != std::string::npos) {
            return std::nullopt;
        }
        return NormalizeCindBatteryPercent(numbers.back());
    }

    if (lowered.find("+ciev") != std::string::npos) {
        // Typical HFP event: +CIEV: <indicator_index>,<value>; for battchg value is usually 0..5.
        return NormalizeCindBatteryPercent(numbers.back());
    }

    if (lowered.find("+xevent") != std::string::npos || lowered.find("+biev") != std::string::npos ||
        lowered.find("battery") != std::string::npos || lowered.find("batt") != std::string::npos) {
        for (auto it = numbers.rbegin(); it != numbers.rend(); ++it) {
            const auto normalized = NormalizeAtBatteryPercent(*it);
            if (normalized.has_value()) {
                return normalized;
            }
        }
    }

    return std::nullopt;
}

std::optional<std::uint8_t> NormalizeCindBatteryPercent(int value) {
    if (value >= 0 && value <= 5) {
        return static_cast<std::uint8_t>(value * 20);
    }
    return NormalizeAtBatteryPercent(value);
}

std::optional<std::uint8_t> TryParseHfpCindBatteryPercent(const std::string& response_text) {
    std::vector<std::string> indicator_names;
    std::vector<int> indicator_values;

    std::istringstream reader(response_text);
    std::string line;
    while (std::getline(reader, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string lowered = ToLowerAscii(line);
        const auto cind_pos = lowered.find("+cind:");
        if (cind_pos == std::string::npos) {
            continue;
        }

        const auto payload = lowered.substr(cind_pos + 6U);
        if (payload.find('"') != std::string::npos) {
            std::size_t cursor = 0;
            while (cursor < payload.size()) {
                const auto open = payload.find('"', cursor);
                if (open == std::string::npos) {
                    break;
                }
                const auto close = payload.find('"', open + 1U);
                if (close == std::string::npos) {
                    break;
                }
                indicator_names.push_back(payload.substr(open + 1U, close - open - 1U));
                cursor = close + 1U;
            }
            continue;
        }

        indicator_values = ExtractIntegersFromText(payload);
    }

    if (indicator_names.empty() || indicator_values.empty()) {
        return std::nullopt;
    }

    for (std::size_t index = 0; index < indicator_names.size(); ++index) {
        const auto& indicator = indicator_names[index];
        if (indicator != "battchg" && indicator.find("battery") == std::string::npos &&
            indicator.find("batt") == std::string::npos) {
            continue;
        }
        if (index >= indicator_values.size()) {
            continue;
        }
        return NormalizeCindBatteryPercent(indicator_values[index]);
    }

    return std::nullopt;
}

void ReplyToHfpAgCommand(SOCKET socket_handle, const std::string& line) {
    if (socket_handle == INVALID_SOCKET || line.empty()) {
        return;
    }

    auto send_reply = [&](const std::string& reply) {
        if (reply.empty()) {
            return;
        }
        send(socket_handle, reply.data(), static_cast<int>(reply.size()), 0);
    };

    const std::string lowered = ToLowerAscii(line);
    if (lowered.rfind("at+brsf=", 0) == 0U) {
        send_reply("\r\n+BRSF: 1024\r\n\r\nOK\r\n");
        send_reply("\r\n+XAPL=ABCD-1234-0100,7\r\n");
        return;
    }
    if (lowered.rfind("at+cind=?", 0) == 0U) {
        send_reply("\r\n+CIND: (\"service\",(0,1)),(\"call\",(0,1)),(\"callsetup\",(0-3)),"
                   "(\"callheld\",(0-2)),(\"battchg\",(0-5))\r\n\r\nOK\r\n");
        return;
    }
    if (lowered.rfind("at+cind?", 0) == 0U) {
        send_reply("\r\n+CIND: 1,0,0,0,5\r\n\r\nOK\r\n");
        return;
    }
    if (lowered.rfind("at+cmer=", 0) == 0U ||
        lowered.rfind("at+bac=", 0) == 0U ||
        lowered.rfind("at+xapl=", 0) == 0U) {
        send_reply("\r\nOK\r\n");
        if (lowered.rfind("at+cmer=", 0) == 0U) {
            send_reply("AT+XEVENT?\r");
            send_reply("AT+XEVENT=BATTERY?\r");
            send_reply("AT+BATT?\r");
            send_reply("AT+BATTERY?\r");
        }
        return;
    }
    if (lowered.rfind("at+iphoneaccev=", 0) == 0U ||
        lowered.rfind("at+xevent", 0) == 0U ||
        lowered.rfind("at+biev", 0) == 0U) {
        send_reply("\r\nOK\r\n");
        send_reply("AT+XEVENT=BATTERY?\r");
        return;
    }
    if (lowered.rfind("at+aplsiri?", 0) == 0U) {
        send_reply("\r\n+APLSIRI: 1\r\n\r\nOK\r\n");
        return;
    }
    if (lowered.rfind("at+", 0) == 0U || lowered == "at") {
        send_reply("\r\nOK\r\n");
    }
}

std::optional<std::uint8_t> TryReadHfpBatteryFromSocket(SOCKET socket_handle) {
    if (socket_handle == INVALID_SOCKET) {
        return std::nullopt;
    }

    constexpr std::array<const char*, 13> kProbeCommands = {
        "AT+XAPL=ABCD-1234-0100,7\r",
        "AT+BRSF=20\r",
        "AT+CMER=3,0,0,1\r",
        "AT+CIND=?\r",
        "AT+CIND?\r",
        "AT+IPHONEACCEV?\r",
        "AT+XEVENT?\r",
        "AT+BIEV?\r",
        "AT+XEVENT=BATTERY?\r",
        "AT+XEVENT=BATTERY\r",
        "AT+BATT?\r",
        "AT+BATTERY?\r",
        "AT+QBAT?\r",
    };

    std::string response_text;

    auto drain_input = [&](int rounds) {
        for (int attempt = 0; attempt < rounds; ++attempt) {
            const auto chunk = ReceiveChunk(socket_handle);
            if (!chunk.has_value()) {
                break;
            }
            response_text.append(reinterpret_cast<const char*>(chunk->data()), chunk->size());
        }
    };

    drain_input(2);

    for (const auto* command : kProbeCommands) {
        const std::size_t command_length = std::strlen(command);
        if (send(socket_handle, command, static_cast<int>(command_length), 0) <= 0) {
            continue;
        }
        if (command_length > 0U && command[command_length - 1U] == '\r') {
            std::string command_crlf(command, command_length);
            command_crlf.push_back('\n');
            send(socket_handle, command_crlf.data(), static_cast<int>(command_crlf.size()), 0);
        }

        const std::size_t before_size = response_text.size();
        std::this_thread::sleep_for(std::chrono::milliseconds(90));
        drain_input(3);
        if (DebugEnabled() && response_text.size() > before_size) {
            const auto delta = response_text.substr(before_size);
            DebugLog("HFP fallback command '" + std::string(command, command + command_length - 1U) +
                     "' response chunk: " + delta);
        }

        std::istringstream reader(response_text);
        std::string line;
        while (std::getline(reader, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const auto parsed = ParseAtBatteryPercentFromLine(line);
            if (parsed.has_value()) {
                DebugLog("HFP fallback battery parsed from line: '" + line + "'");
                return parsed;
            }
        }

        const auto cind_parsed = TryParseHfpCindBatteryPercent(response_text);
        if (cind_parsed.has_value()) {
            DebugLog("HFP fallback battery parsed from +CIND");
            return cind_parsed;
        }
    }

    return std::nullopt;
}

std::optional<std::uint8_t> TryReadGenericClassicHfpBattery(std::uint64_t bluetooth_address) {
    if (bluetooth_address <= 0xFFFFULL) {
        return std::nullopt;
    }

    DebugLog("Generic HFP fallback: attempting RFCOMM connection to address=" + std::to_string(bluetooth_address));

    ScopedWsa wsa;
    if (!wsa.started()) {
        DebugLog("Generic HFP fallback: WSAStartup failed");
        return std::nullopt;
    }

    SOCKET socket_handle = INVALID_SOCKET;
    SOCKADDR_BTH address{};
    address.addressFamily = AF_BTH;
    address.btAddr = bluetooth_address;
    address.port = BT_PORT_ANY;

    auto try_connect_hfp_guid = [&](bool secure_mode, const char* mode_suffix) -> bool {
        const SOCKET candidate_socket = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
        if (candidate_socket == INVALID_SOCKET) {
            return false;
        }

        const ULONG secure_transport = secure_mode ? TRUE : FALSE;
        setsockopt(candidate_socket, SOL_RFCOMM, SO_BTH_AUTHENTICATE,
                   reinterpret_cast<const char*>(&secure_transport), sizeof(secure_transport));
        setsockopt(candidate_socket, SOL_RFCOMM, SO_BTH_ENCRYPT,
                   reinterpret_cast<const char*>(&secure_transport), sizeof(secure_transport));

        const int timeout_ms = 320;
        setsockopt(candidate_socket, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
        setsockopt(candidate_socket, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

        auto service_address = address;
        service_address.serviceClassId = kHandsfreeAudioGatewayServiceUuid;
        if (!ConnectWithTimeout(candidate_socket, service_address, timeout_ms)) {
            closesocket(candidate_socket);
            return false;
        }

        socket_handle = candidate_socket;
        DebugLog(std::string("Generic HFP fallback: RFCOMM connected via HFP-111E") + mode_suffix);
        return true;
    };

    auto try_connect_port = [&](std::uint32_t port, bool secure_mode, const char* mode_suffix) -> bool {
        const SOCKET candidate_socket = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
        if (candidate_socket == INVALID_SOCKET) {
            return false;
        }

        const ULONG secure_transport = secure_mode ? TRUE : FALSE;
        setsockopt(candidate_socket, SOL_RFCOMM, SO_BTH_AUTHENTICATE,
                   reinterpret_cast<const char*>(&secure_transport), sizeof(secure_transport));
        setsockopt(candidate_socket, SOL_RFCOMM, SO_BTH_ENCRYPT,
                   reinterpret_cast<const char*>(&secure_transport), sizeof(secure_transport));

        const int timeout_ms = 220;
        setsockopt(candidate_socket, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
        setsockopt(candidate_socket, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

        auto service_address = address;
        service_address.serviceClassId = GUID{};
        service_address.port = port;
        if (!ConnectWithTimeout(candidate_socket, service_address, timeout_ms)) {
            closesocket(candidate_socket);
            return false;
        }

        socket_handle = candidate_socket;
        DebugLog("Generic HFP fallback: RFCOMM connected via port-" + std::to_string(port) + mode_suffix);
        return true;
    };

    bool connected = try_connect_hfp_guid(true, "") || try_connect_hfp_guid(false, "-insecure");
    if (!connected) {
        const auto channels =
            DiscoverRfcommChannelsFromSdp(bluetooth_address, &kHandsfreeAudioGatewayServiceUuid, false);
        for (const auto& channel : channels) {
            if (try_connect_port(channel.port, true, "") ||
                try_connect_port(channel.port, false, "-insecure")) {
                connected = true;
                break;
            }
        }
    }
    if (!connected) {
        connected = try_connect_port(2U, true, "") || try_connect_port(2U, false, "-insecure");
    }
    if (!connected) {
        return std::nullopt;
    }

    const auto battery = TryReadHfpBatteryFromSocket(socket_handle);
    closesocket(socket_handle);
    if (battery.has_value()) {
        DebugLog("Generic HFP fallback: battery main=" + std::to_string(*battery));
    } else {
        DebugLog("Generic HFP fallback: battery was not reported");
    }
    return battery;
}

std::vector<BatteryReading> TryReadHfpAgBatteryFromSocket(SOCKET socket_handle) {
    std::vector<BatteryReading> readings;
    if (socket_handle == INVALID_SOCKET) {
        return readings;
    }

    std::optional<std::uint8_t> main_battery;
    std::string text_buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3200);

    while (std::chrono::steady_clock::now() < deadline) {
        const auto chunk = ReceiveChunk(socket_handle);
        if (!chunk.has_value() || chunk->empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(55));
            continue;
        }

        const std::string chunk_text(reinterpret_cast<const char*>(chunk->data()), chunk->size());
        text_buffer.append(chunk_text);
        if (DebugEnabled()) {
            DebugLog("HFP AG rx='" + chunk_text + "'");
        }

        std::size_t start = 0;
        while (start < text_buffer.size()) {
            const auto line_end = text_buffer.find_first_of("\r\n", start);
            if (line_end == std::string::npos) {
                break;
            }

            std::string line = text_buffer.substr(start, line_end - start);
            start = line_end + 1U;
            while (start < text_buffer.size() &&
                   (text_buffer[start] == '\r' || text_buffer[start] == '\n')) {
                ++start;
            }

            if (line.empty()) {
                continue;
            }

            ReplyToHfpAgCommand(socket_handle, line);

            if (const auto snapshot = ExtractZmiSerialTextSnapshot(line); snapshot.has_value()) {
                readings = BuildXiaomiBatteryReadings(*snapshot);
                if (!readings.empty()) {
                    DebugLog("HFP AG battery triplet parsed from line: '" + line + "'");
                    return readings;
                }
            }

            if (const auto parsed = ParseAtBatteryPercentFromLine(line); parsed.has_value()) {
                main_battery = parsed;
            }
        }

        if (start > 0U) {
            text_buffer.erase(0, start);
        }
    }

    if (main_battery.has_value()) {
        readings.push_back(BatteryReading{"main", *main_battery});
    }

    return readings;
}

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

bool HasAnyBattery(const XiaomiBatterySnapshot& snapshot) {
    return snapshot.left.has_value() || snapshot.right.has_value() || snapshot.case_level.has_value();
}

int SnapshotValueOrMissing(const std::optional<std::uint8_t>& value) {
    return value.has_value() ? static_cast<int>(*value) : -1;
}

std::optional<std::uint8_t> SnapshotValueFromInt(int value) {
    if (value < 0 || value > 100) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(value);
}

struct XiaomiPersistentCacheEntry {
    std::int64_t updated_at_unix = 0;
    XiaomiBatterySnapshot snapshot;
};

using XiaomiPersistentCacheMap = std::unordered_map<std::uint64_t, XiaomiPersistentCacheEntry>;

XiaomiPersistentCacheMap LoadPersistentXiaomiCache() {
    XiaomiPersistentCacheMap cache;

    const auto cache_file = XiaomiCacheFilePath();
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

void SavePersistentXiaomiCache(const XiaomiPersistentCacheMap& cache) {
    const auto cache_file = XiaomiCacheFilePath();
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

XiaomiPersistentCacheMap& PersistentXiaomiCacheStore() {
    static XiaomiPersistentCacheMap cache = LoadPersistentXiaomiCache();
    return cache;
}

void PutPersistentXiaomiSnapshot(std::uint64_t address, const XiaomiBatterySnapshot& snapshot) {
    if (!PersistentXiaomiCacheWriteEnabled() || !HasAnyBattery(snapshot)) {
        return;
    }

    auto& cache = PersistentXiaomiCacheStore();
    XiaomiPersistentCacheEntry entry;
    entry.updated_at_unix = CurrentUnixSeconds();
    entry.snapshot = snapshot;
    cache[address] = entry;
    SavePersistentXiaomiCache(cache);
}

std::optional<XiaomiBatterySnapshot> GetPersistentXiaomiSnapshot(std::uint64_t address) {
    if (!PersistentXiaomiCacheReadEnabled()) {
        return std::nullopt;
    }

    auto& cache = PersistentXiaomiCacheStore();
    const auto found = cache.find(address);
    if (found == cache.end()) {
        return std::nullopt;
    }

    const auto now = CurrentUnixSeconds();
    const auto age_seconds = now - found->second.updated_at_unix;
    const auto ttl_seconds = static_cast<std::int64_t>(XiaomiCacheTtlMinutes()) * 60LL;
    if (age_seconds < 0 || age_seconds > ttl_seconds) {
        cache.erase(found);
        SavePersistentXiaomiCache(cache);
        return std::nullopt;
    }

    if (!HasAnyBattery(found->second.snapshot)) {
        return std::nullopt;
    }

    return found->second.snapshot;
}

std::vector<BatteryReading> TryReadXiaomiClassicBattery(std::uint64_t bluetooth_address,
                                                        bool enable_dynamic_port_scan) {
    std::vector<BatteryReading> readings;

    DebugLog("Xiaomi classic fallback: attempting RFCOMM connection to address=" + std::to_string(bluetooth_address));

    ScopedWsa wsa;
    if (!wsa.started()) {
        DebugLog("Xiaomi classic fallback: WSAStartup failed");
        return readings;
    }

    SOCKET socket_handle = INVALID_SOCKET;
    SOCKADDR_BTH address{};
    address.addressFamily = AF_BTH;
    address.btAddr = bluetooth_address;
    address.port = BT_PORT_ANY;
    std::string connected_path;

    auto try_connect = [&](const GUID& service_uuid, const char* service_name) -> bool {
        auto connect_once = [&](bool secure_mode, const char* mode_suffix) -> bool {
            const SOCKET candidate_socket = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
            if (candidate_socket == INVALID_SOCKET) {
                DebugLog("Xiaomi classic fallback: socket() failed for " + std::string(service_name) +
                         " error=" + std::to_string(WSAGetLastError()));
                return false;
            }

            const ULONG secure_transport = secure_mode ? TRUE : FALSE;
            setsockopt(candidate_socket, SOL_RFCOMM, SO_BTH_AUTHENTICATE,
                       reinterpret_cast<const char*>(&secure_transport), sizeof(secure_transport));
            setsockopt(candidate_socket, SOL_RFCOMM, SO_BTH_ENCRYPT,
                       reinterpret_cast<const char*>(&secure_transport), sizeof(secure_transport));

            const int timeout_ms = 280;
            setsockopt(candidate_socket, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
            setsockopt(candidate_socket, SOL_SOCKET, SO_SNDTIMEO,
                       reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

            auto service_address = address;
            service_address.serviceClassId = service_uuid;

            if (!ConnectWithTimeout(candidate_socket, service_address, timeout_ms)) {
                DebugLog("Xiaomi classic fallback: connect(" + std::string(service_name) + mode_suffix +
                         ") failed error=" + std::to_string(WSAGetLastError()));
                closesocket(candidate_socket);
                return false;
            }

            socket_handle = candidate_socket;
            connected_path = std::string(service_name) + mode_suffix;
            DebugLog("Xiaomi classic fallback: RFCOMM connected via " + connected_path);
            return true;
        };

        return connect_once(true, "") || connect_once(false, "-insecure");
    };

    auto try_connect_port = [&](std::uint32_t port, const char* name, int timeout_ms = 280) -> bool {
        auto connect_once = [&](bool secure_mode, const char* mode_suffix) -> bool {
            const SOCKET candidate_socket = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
            if (candidate_socket == INVALID_SOCKET) {
                DebugLog("Xiaomi classic fallback: socket() failed for " + std::string(name) +
                         " error=" + std::to_string(WSAGetLastError()));
                return false;
            }

            const ULONG secure_transport = secure_mode ? TRUE : FALSE;
            setsockopt(candidate_socket, SOL_RFCOMM, SO_BTH_AUTHENTICATE,
                       reinterpret_cast<const char*>(&secure_transport), sizeof(secure_transport));
            setsockopt(candidate_socket, SOL_RFCOMM, SO_BTH_ENCRYPT,
                       reinterpret_cast<const char*>(&secure_transport), sizeof(secure_transport));

            setsockopt(candidate_socket, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
            setsockopt(candidate_socket, SOL_SOCKET, SO_SNDTIMEO,
                       reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

            auto service_address = address;
            service_address.serviceClassId = GUID{};
            service_address.port = port;

            if (!ConnectWithTimeout(candidate_socket, service_address, timeout_ms)) {
                DebugLog("Xiaomi classic fallback: connect(" + std::string(name) + mode_suffix +
                         ") failed error=" + std::to_string(WSAGetLastError()));
                closesocket(candidate_socket);
                return false;
            }

            socket_handle = candidate_socket;
            connected_path = std::string(name) + mode_suffix;
            DebugLog("Xiaomi classic fallback: RFCOMM connected via " + connected_path);
            return true;
        };

        return connect_once(true, "") || connect_once(false, "-insecure");
    };

    bool connected = try_connect(kXiaomiDeviceCtrlServiceUuid, "FD2D") ||
                     try_connect(kBluetoothSerialPortServiceUuid, "SPP-1101") ||
                     try_connect(kZmiPurPodsSerialServiceUuid, "ZMI-1101") ||
                     try_connect(kHandsfreeAudioGatewayServiceUuid, "HFP-111E") ||
                     try_connect_port(15, "RFCOMM-port-15");

    if (!connected && enable_dynamic_port_scan) {
        std::set<std::uint32_t> sdp_ports_attempted;
        auto try_sdp = [&](const GUID* service_filter, const char* source_name, bool flush_cache) {
            const auto channels = DiscoverRfcommChannelsFromSdp(bluetooth_address, service_filter, flush_cache);
            if (!channels.empty()) {
                DebugLog("Xiaomi classic fallback: SDP discovered channels=" + std::to_string(channels.size()) +
                         " source=" + std::string(source_name));
            }

            for (const auto& channel : channels) {
                if (!sdp_ports_attempted.insert(channel.port).second) {
                    continue;
                }

                std::string label = std::string(source_name) + "-port-" + std::to_string(channel.port);
                if (!channel.instance_name.empty()) {
                    label += " '" + channel.instance_name + "'";
                }

                if (try_connect_port(channel.port, label.c_str(), 180)) {
                    DebugLog("Xiaomi classic fallback: SDP matched port=" + std::to_string(channel.port) +
                             " source=" + std::string(source_name));
                    connected = true;
                    return;
                }
            }
        };

        try_sdp(&kZmiPurPodsSerialServiceUuid, "SDP-ZMI-1101", true);
        if (!connected) {
            try_sdp(&kBluetoothSerialPortServiceUuid, "SDP-SPP-1101", false);
        }
        if (!connected) {
            try_sdp(&kHandsfreeAudioGatewayServiceUuid, "SDP-HFP-111E", false);
        }
        if (!connected) {
            try_sdp(nullptr, "SDP-ANY", false);
        }

        if (!connected) {
        constexpr std::array<std::uint32_t, 24> kZmiCandidatePorts = {
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
            13, 14, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25};
        for (const auto port : kZmiCandidatePorts) {
            if (sdp_ports_attempted.contains(port)) {
                continue;
            }
            std::string port_name = "RFCOMM-port-" + std::to_string(port);
            if (try_connect_port(port, port_name.c_str(), 110)) {
                DebugLog("Xiaomi classic fallback: dynamic port scan matched port=" + std::to_string(port));
                connected = true;
                break;
            }
        }
        }
    }

    if (!connected) {
        return readings;
    }

    const bool zmi_serial_path =
        connected_path.find("ZMI-1101") != std::string::npos ||
        connected_path.find("SDP-ZMI-1101") != std::string::npos;
    const bool try_zmi_pre_auth_probe = zmi_serial_path || enable_dynamic_port_scan;
    std::vector<BatteryReading> partial_pre_auth_readings;
    if (try_zmi_pre_auth_probe) {
        auto pre_auth_readings = TryReadZmiSerialBatteryFromSocket(socket_handle);
        if (!pre_auth_readings.empty()) {
            if (HasUsefulXiaomiTwsReadings(pre_auth_readings)) {
                readings = std::move(pre_auth_readings);
                DebugLog("Xiaomi classic fallback: ZMI pre-auth serial probe succeeded, entries=" +
                         std::to_string(readings.size()));
                closesocket(socket_handle);
                return readings;
            }

            partial_pre_auth_readings = std::move(pre_auth_readings);
            DebugLog("Xiaomi classic fallback: ZMI pre-auth serial probe yielded partial entries=" +
                     std::to_string(partial_pre_auth_readings.size()) +
                     ", continue auth flow");
        }
    }

    // Restore a less aggressive socket timeout before auth/status flow.
    const int io_timeout_ms = 260;
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&io_timeout_ms), sizeof(io_timeout_ms));
    setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&io_timeout_ms), sizeof(io_timeout_ms));

    const bool likely_hfp_transport =
        connected_path.find("HFP") != std::string::npos ||
        connected_path.find("SDP-HFP") != std::string::npos;
    if (enable_dynamic_port_scan && likely_hfp_transport) {
        auto hfp_triplet = TryReadHfpAgBatteryFromSocket(socket_handle);
        if (HasUsefulXiaomiTwsReadings(hfp_triplet)) {
            readings = std::move(hfp_triplet);
            closesocket(socket_handle);
            return readings;
        }

        const auto hfp_main = TryReadHfpBatteryFromSocket(socket_handle);
        if (hfp_main.has_value()) {
            readings.push_back(BatteryReading{"main", *hfp_main});
        }
        if (readings.empty() && !partial_pre_auth_readings.empty()) {
            readings = partial_pre_auth_readings;
        }
        closesocket(socket_handle);
        return readings;
    }

    std::uint8_t sequence = 0;

    const auto challenge = GenerateRandomChallenge();
    XiaomiMessage auth_start;
    auth_start.type = XiaomiMessageType::kPhoneRequest;
    auth_start.opcode = XiaomiOpcode::kAuthChallenge;
    auth_start.sequence = sequence++;
    auth_start.payload.push_back(0x01);
    auth_start.payload.insert(auth_start.payload.end(), challenge.begin(), challenge.end());
    if (!SendAll(socket_handle, EncodeXiaomiMessage(auth_start))) {
        DebugLog("Xiaomi classic fallback: failed to send auth challenge");
        if (!partial_pre_auth_readings.empty()) {
            readings = partial_pre_auth_readings;
        }
        if (enable_dynamic_port_scan) {
            auto hfp_triplet = TryReadHfpAgBatteryFromSocket(socket_handle);
            if (HasUsefulXiaomiTwsReadings(hfp_triplet)) {
                readings = std::move(hfp_triplet);
            }
            if (readings.empty()) {
                const auto hfp_main = TryReadHfpBatteryFromSocket(socket_handle);
                if (hfp_main.has_value()) {
                    readings.push_back(BatteryReading{"main", *hfp_main});
                }
            }
        }
        closesocket(socket_handle);
        return readings;
    }

    bool init_requests_sent = false;
    std::optional<XiaomiBatterySnapshot> device_info_snapshot;
    std::optional<XiaomiBatterySnapshot> status_snapshot;
    std::optional<XiaomiBatterySnapshot> generic_snapshot;
    std::optional<std::chrono::steady_clock::time_point> device_info_received_at;
    std::optional<std::chrono::steady_clock::time_point> report_status_seen_at;
    std::vector<std::uint8_t> rx_buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1600);

    while (std::chrono::steady_clock::now() < deadline) {
        if (device_info_snapshot.has_value() && device_info_received_at.has_value() &&
            std::chrono::steady_clock::now() - *device_info_received_at > std::chrono::milliseconds(500)) {
            break;
        }
        if (!status_snapshot.has_value() && report_status_seen_at.has_value() &&
            std::chrono::steady_clock::now() - *report_status_seen_at > std::chrono::milliseconds(900)) {
            break;
        }

        const auto chunk = ReceiveChunk(socket_handle);
        if (!chunk.has_value()) {
            continue;
        }

        const auto raw_extracted = ExtractPreferredXiaomiBatterySnapshot(*chunk);
        if (raw_extracted.has_value()) {
            if (generic_snapshot.has_value()) {
                generic_snapshot = MergeXiaomiSnapshots(*raw_extracted, *generic_snapshot);
            } else {
                generic_snapshot = raw_extracted;
            }
        }
        rx_buffer.insert(rx_buffer.end(), chunk->begin(), chunk->end());

        const auto messages = DecodeXiaomiMessages(&rx_buffer);
        for (const auto& message : messages) {
            const auto type = message.type;
            const auto opcode = message.opcode;

            if (DebugEnabled()) {
                DebugLog("Xiaomi message type=" + ByteToHex(static_cast<std::uint8_t>(type)) +
                         " opcode=" + ByteToHex(static_cast<std::uint8_t>(opcode)) +
                         " seq=" + std::to_string(message.sequence) +
                         " payload=" + BytesToHex(message.payload));
            }

            if (const auto mode_code =
                    ParseXiaomiNoiseModeCode(static_cast<std::uint8_t>(opcode), message.payload);
                mode_code.has_value()) {
                const auto submode_code =
                    static_cast<std::uint8_t>(opcode) == 0xF4U
                        ? ParseXiaomiNoiseSubmodeCodeFromF4Payload(message.payload)
                        : std::optional<std::uint8_t>{};
                PutXiaomiModeCacheEntry(bluetooth_address, *mode_code, submode_code);
                DebugLog("Xiaomi mode candidate code=" + std::to_string(*mode_code) +
                         " text='" + XiaomiNoiseModeCodeToText(*mode_code) + "'");
            }

            if (opcode == XiaomiOpcode::kAuthChallenge) {
                if (type == XiaomiMessageType::kResponse) {
                    XiaomiMessage confirm;
                    confirm.type = XiaomiMessageType::kPhoneRequest;
                    confirm.opcode = XiaomiOpcode::kAuthConfirm;
                    confirm.sequence = sequence++;
                    confirm.payload = {0x01, 0x00};
                    SendAll(socket_handle, EncodeXiaomiMessage(confirm));
                    continue;
                }

                if (type == XiaomiMessageType::kEarbudsRequest && message.payload.size() >= 17U) {
                    std::array<std::uint8_t, 16> remote_challenge{};
                    std::copy_n(message.payload.begin() + 1, 16, remote_challenge.begin());
                    const auto response = ComputeXiaomiChallengeResponse(remote_challenge);

                    XiaomiMessage challenge_response;
                    challenge_response.type = XiaomiMessageType::kResponse;
                    challenge_response.opcode = XiaomiOpcode::kAuthChallenge;
                    challenge_response.sequence = message.sequence;
                    challenge_response.payload.push_back(0x01);
                    challenge_response.payload.insert(challenge_response.payload.end(), response.begin(), response.end());
                    SendAll(socket_handle, EncodeXiaomiMessage(challenge_response));
                    continue;
                }
            }

            if (opcode == XiaomiOpcode::kAuthConfirm && type == XiaomiMessageType::kEarbudsRequest) {
                XiaomiMessage ack;
                ack.type = XiaomiMessageType::kResponse;
                ack.opcode = XiaomiOpcode::kAuthConfirm;
                ack.sequence = message.sequence;
                ack.payload = {0x01};
                SendAll(socket_handle, EncodeXiaomiMessage(ack));

                if (!init_requests_sent) {
                    XiaomiMessage info_request;
                    info_request.type = XiaomiMessageType::kPhoneRequest;
                    info_request.opcode = XiaomiOpcode::kGetDeviceInfo;
                    info_request.sequence = sequence++;
                    info_request.payload = {0xFF, 0xFF, 0xFF, 0xFF};
                    SendAll(socket_handle, EncodeXiaomiMessage(info_request));

                    XiaomiMessage run_info_request;
                    run_info_request.type = XiaomiMessageType::kPhoneRequest;
                    run_info_request.opcode = XiaomiOpcode::kGetDeviceRunInfo;
                    run_info_request.sequence = sequence++;
                    run_info_request.payload = {0xFF, 0xFF, 0xFF, 0xFF};
                    SendAll(socket_handle, EncodeXiaomiMessage(run_info_request));

                    init_requests_sent = true;
                }
                continue;
            }

            if (opcode == XiaomiOpcode::kGetDeviceInfo && !message.payload.empty()) {
                const auto extracted =
                    ExtractBatterySnapshotFromXiaomiPayload(
                        message.payload, std::optional<std::uint8_t>{static_cast<std::uint8_t>(0x07U)});
                if (extracted.has_value()) {
                    device_info_snapshot = extracted;
                    device_info_received_at = std::chrono::steady_clock::now();
                }
                continue;
            }

            if (opcode == XiaomiOpcode::kReportStatus && !message.payload.empty()) {
                report_status_seen_at = std::chrono::steady_clock::now();

                if (type == XiaomiMessageType::kEarbudsNotify) {
                    XiaomiMessage report_ack;
                    report_ack.type = XiaomiMessageType::kResponse;
                    report_ack.opcode = XiaomiOpcode::kReportStatus;
                    report_ack.sequence = message.sequence;
                    report_ack.payload = {};
                    SendAll(socket_handle, EncodeXiaomiMessage(report_ack));
                }

                const auto extracted =
                    ExtractBatterySnapshotFromXiaomiPayload(
                        message.payload, std::optional<std::uint8_t>{static_cast<std::uint8_t>(0x00U)});
                if (extracted.has_value()) {
                    status_snapshot = extracted;
                    break;
                }
                continue;
            }

            if (!message.payload.empty()) {
                const auto extracted = ExtractPreferredXiaomiBatterySnapshot(message.payload);
                if (extracted.has_value()) {
                    if (generic_snapshot.has_value()) {
                        generic_snapshot = MergeXiaomiSnapshots(*extracted, *generic_snapshot);
                    } else {
                        generic_snapshot = extracted;
                    }
                }
            }
        }

        if (status_snapshot.has_value()) {
            break;
        }
    }

    if (status_snapshot.has_value() || device_info_snapshot.has_value() || generic_snapshot.has_value()) {
        XiaomiBatterySnapshot merged{};
        if (status_snapshot.has_value() && device_info_snapshot.has_value()) {
            merged = MergeXiaomiSnapshots(*status_snapshot, *device_info_snapshot);
        } else if (status_snapshot.has_value()) {
            merged = *status_snapshot;
        } else if (device_info_snapshot.has_value()) {
            merged = *device_info_snapshot;
        } else {
            merged = *generic_snapshot;
        }

        if (generic_snapshot.has_value()) {
            const bool has_preferred_snapshot = status_snapshot.has_value() || device_info_snapshot.has_value();
            if (!has_preferred_snapshot) {
                merged = MergeXiaomiSnapshots(merged, *generic_snapshot);
            } else if (DebugEnabled()) {
                DebugLog("Xiaomi classic fallback: generic snapshot ignored because preferred snapshot is available");
            }
        }

        readings = BuildXiaomiBatteryReadings(merged);
        if (!HasUsefulXiaomiTwsReadings(readings) && !partial_pre_auth_readings.empty()) {
            if (XiaomiReadingsRichnessScore(partial_pre_auth_readings) > XiaomiReadingsRichnessScore(readings)) {
                readings = partial_pre_auth_readings;
            }
        }
        const std::string left_text = merged.left.has_value() ? std::to_string(*merged.left) : "na";
        const std::string right_text = merged.right.has_value() ? std::to_string(*merged.right) : "na";
        const std::string case_text = merged.case_level.has_value() ? std::to_string(*merged.case_level) : "na";
        DebugLog("Xiaomi classic fallback: battery merged left=" + left_text +
                 " right=" + right_text + " case=" + case_text);
    } else {
        if (zmi_serial_path || enable_dynamic_port_scan) {
            auto zmi_serial_readings = TryReadZmiSerialBatteryFromSocket(socket_handle);
            if (!zmi_serial_readings.empty()) {
                readings = std::move(zmi_serial_readings);
                DebugLog("Xiaomi classic fallback: ZMI serial pattern fallback succeeded, entries=" +
                         std::to_string(readings.size()));
            }
        }

        if (readings.empty()) {
            DebugLog("Xiaomi classic fallback: timeout without battery payload");

            const bool likely_hfp_path = connected_path.find("HFP") != std::string::npos;
            if (enable_dynamic_port_scan && likely_hfp_path) {
                auto hfp_triplet = TryReadHfpAgBatteryFromSocket(socket_handle);
                if (!hfp_triplet.empty()) {
                    readings = std::move(hfp_triplet);
                }
            }

            std::optional<std::uint8_t> hfp_battery;
            if (readings.empty() && likely_hfp_path) {
                hfp_battery = TryReadHfpBatteryFromSocket(socket_handle);
            } else if (readings.empty()) {
                const SOCKET hfp_socket = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
                if (hfp_socket != INVALID_SOCKET) {
                    const int timeout_ms = 240;
                    setsockopt(hfp_socket, SOL_SOCKET, SO_RCVTIMEO,
                               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
                    setsockopt(hfp_socket, SOL_SOCKET, SO_SNDTIMEO,
                               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

                    auto hfp_address = address;
                    hfp_address.port = BT_PORT_ANY;
                    hfp_address.serviceClassId = kHandsfreeAudioGatewayServiceUuid;
                    if (ConnectWithTimeout(hfp_socket, hfp_address, timeout_ms)) {
                        if (enable_dynamic_port_scan) {
                            auto hfp_triplet = TryReadHfpAgBatteryFromSocket(hfp_socket);
                            if (!hfp_triplet.empty()) {
                                readings = std::move(hfp_triplet);
                            }
                        }
                        if (readings.empty()) {
                            hfp_battery = TryReadHfpBatteryFromSocket(hfp_socket);
                        }
                    }
                    closesocket(hfp_socket);
                }
            }

            if (readings.empty() && hfp_battery.has_value()) {
                readings.push_back(BatteryReading{"main", *hfp_battery});
                DebugLog("Xiaomi classic fallback: HFP battery main=" + std::to_string(*hfp_battery));
            }
        }
    }

    if (!HasUsefulXiaomiTwsReadings(readings) && !partial_pre_auth_readings.empty()) {
        if (XiaomiReadingsRichnessScore(partial_pre_auth_readings) > XiaomiReadingsRichnessScore(readings)) {
            readings = partial_pre_auth_readings;
        }
    }

    closesocket(socket_handle);
    return readings;
}

struct XiaomiProbeCommand {
    const char* label = "";
    XiaomiMessageType type = XiaomiMessageType::kPhoneRequest;
    std::uint8_t opcode = 0;
    std::vector<std::uint8_t> payload;
    int pause_ms = 1800;
};

bool ConnectXiaomiControlSocket(std::uint64_t bluetooth_address, SOCKET* socket_handle, std::string* connected_path) {
    if (socket_handle == nullptr || connected_path == nullptr) {
        return false;
    }

    *socket_handle = INVALID_SOCKET;
    connected_path->clear();

    SOCKADDR_BTH address{};
    address.addressFamily = AF_BTH;
    address.btAddr = bluetooth_address;
    address.port = BT_PORT_ANY;

    auto try_connect = [&](const GUID& service_uuid, const char* service_name) -> bool {
        auto connect_once = [&](bool secure_mode, const char* mode_suffix) -> bool {
            const SOCKET candidate_socket = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
            if (candidate_socket == INVALID_SOCKET) {
                return false;
            }

            const ULONG secure_transport = secure_mode ? TRUE : FALSE;
            setsockopt(candidate_socket, SOL_RFCOMM, SO_BTH_AUTHENTICATE,
                       reinterpret_cast<const char*>(&secure_transport), sizeof(secure_transport));
            setsockopt(candidate_socket, SOL_RFCOMM, SO_BTH_ENCRYPT,
                       reinterpret_cast<const char*>(&secure_transport), sizeof(secure_transport));

            const int timeout_ms = 320;
            setsockopt(candidate_socket, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
            setsockopt(candidate_socket, SOL_SOCKET, SO_SNDTIMEO,
                       reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

            auto service_address = address;
            service_address.serviceClassId = service_uuid;
            if (!ConnectWithTimeout(candidate_socket, service_address, timeout_ms)) {
                closesocket(candidate_socket);
                return false;
            }

            *socket_handle = candidate_socket;
            *connected_path = std::string(service_name) + mode_suffix;
            return true;
        };

        return connect_once(true, "") || connect_once(false, "-insecure");
    };

    return try_connect(kXiaomiDeviceCtrlServiceUuid, "FD2D") ||
           try_connect(kBluetoothSerialPortServiceUuid, "SPP-1101") ||
           try_connect(kZmiPurPodsSerialServiceUuid, "ZMI-1101");
}

bool RunXiaomiAuthHandshake(SOCKET socket_handle, std::uint8_t* next_sequence) {
    if (socket_handle == INVALID_SOCKET || next_sequence == nullptr) {
        return false;
    }

    std::uint8_t sequence = 0;
    const auto challenge = GenerateRandomChallenge();
    XiaomiMessage auth_start;
    auth_start.type = XiaomiMessageType::kPhoneRequest;
    auth_start.opcode = XiaomiOpcode::kAuthChallenge;
    auth_start.sequence = sequence++;
    auth_start.payload.push_back(0x01);
    auth_start.payload.insert(auth_start.payload.end(), challenge.begin(), challenge.end());
    if (!SendAll(socket_handle, EncodeXiaomiMessage(auth_start))) {
        return false;
    }

    std::vector<std::uint8_t> rx_buffer;
    bool init_requests_sent = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1800);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto chunk = ReceiveChunk(socket_handle);
        if (!chunk.has_value()) {
            continue;
        }

        rx_buffer.insert(rx_buffer.end(), chunk->begin(), chunk->end());
        const auto messages = DecodeXiaomiMessages(&rx_buffer);
        for (const auto& message : messages) {
            if (DebugEnabled()) {
                DebugLog("Probe auth rx type=" + ByteToHex(static_cast<std::uint8_t>(message.type)) +
                         " opcode=" + ByteToHex(static_cast<std::uint8_t>(message.opcode)) +
                         " seq=" + std::to_string(message.sequence) +
                         " payload=" + BytesToHex(message.payload));
            }

            if (message.opcode == XiaomiOpcode::kAuthChallenge) {
                if (message.type == XiaomiMessageType::kResponse) {
                    XiaomiMessage confirm;
                    confirm.type = XiaomiMessageType::kPhoneRequest;
                    confirm.opcode = XiaomiOpcode::kAuthConfirm;
                    confirm.sequence = sequence++;
                    confirm.payload = {0x01, 0x00};
                    SendAll(socket_handle, EncodeXiaomiMessage(confirm));
                    continue;
                }

                if (message.type == XiaomiMessageType::kEarbudsRequest && message.payload.size() >= 17U) {
                    std::array<std::uint8_t, 16> remote_challenge{};
                    std::copy_n(message.payload.begin() + 1, 16, remote_challenge.begin());
                    const auto response = ComputeXiaomiChallengeResponse(remote_challenge);

                    XiaomiMessage challenge_response;
                    challenge_response.type = XiaomiMessageType::kResponse;
                    challenge_response.opcode = XiaomiOpcode::kAuthChallenge;
                    challenge_response.sequence = message.sequence;
                    challenge_response.payload.push_back(0x01);
                    challenge_response.payload.insert(challenge_response.payload.end(), response.begin(), response.end());
                    SendAll(socket_handle, EncodeXiaomiMessage(challenge_response));
                    continue;
                }
            }

            if (message.opcode == XiaomiOpcode::kAuthConfirm && message.type == XiaomiMessageType::kEarbudsRequest) {
                XiaomiMessage ack;
                ack.type = XiaomiMessageType::kResponse;
                ack.opcode = XiaomiOpcode::kAuthConfirm;
                ack.sequence = message.sequence;
                ack.payload = {0x01};
                SendAll(socket_handle, EncodeXiaomiMessage(ack));

                if (!init_requests_sent) {
                    XiaomiMessage info_request;
                    info_request.type = XiaomiMessageType::kPhoneRequest;
                    info_request.opcode = XiaomiOpcode::kGetDeviceInfo;
                    info_request.sequence = sequence++;
                    info_request.payload = {0xFF, 0xFF, 0xFF, 0xFF};
                    SendAll(socket_handle, EncodeXiaomiMessage(info_request));

                    XiaomiMessage run_info_request;
                    run_info_request.type = XiaomiMessageType::kPhoneRequest;
                    run_info_request.opcode = XiaomiOpcode::kGetDeviceRunInfo;
                    run_info_request.sequence = sequence++;
                    run_info_request.payload = {0xFF, 0xFF, 0xFF, 0xFF};
                    SendAll(socket_handle, EncodeXiaomiMessage(run_info_request));
                    init_requests_sent = true;
                }

                *next_sequence = sequence;
                return true;
            }
        }
    }

    return false;
}

std::vector<XiaomiProbeCommand> BuildXiaomiNoiseProbeCommands() {
    return {
        {"candidate 01", XiaomiMessageType::kPhoneRequest, 0x03, {0x01, 0x00}},
        {"candidate 02", XiaomiMessageType::kPhoneRequest, 0x03, {0x01, 0x01}},
        {"candidate 03", XiaomiMessageType::kPhoneRequest, 0x03, {0x01, 0x02}},
        {"candidate 04", XiaomiMessageType::kPhoneRequest, 0x03, {0x02, 0x00}},
        {"candidate 05", XiaomiMessageType::kPhoneRequest, 0x03, {0x02, 0x01}},
        {"candidate 06", XiaomiMessageType::kPhoneRequest, 0x03, {0x02, 0x02}},
        {"candidate 07", XiaomiMessageType::kPhoneRequest, 0x04, {0x01, 0x00}},
        {"candidate 08", XiaomiMessageType::kPhoneRequest, 0x04, {0x01, 0x01}},
        {"candidate 09", XiaomiMessageType::kPhoneRequest, 0x04, {0x01, 0x02}},
        {"candidate 10", XiaomiMessageType::kPhoneRequest, 0x05, {0x01, 0x00}},
        {"candidate 11", XiaomiMessageType::kPhoneRequest, 0x05, {0x01, 0x01}},
        {"candidate 12", XiaomiMessageType::kPhoneRequest, 0x05, {0x01, 0x02}},
        {"candidate 13", XiaomiMessageType::kPhoneRequest, 0x06, {0x00}},
        {"candidate 14", XiaomiMessageType::kPhoneRequest, 0x06, {0x01}},
        {"candidate 15", XiaomiMessageType::kPhoneRequest, 0x06, {0x02}},
        {"candidate 16", XiaomiMessageType::kPhoneRequest, 0x08, {0x01, 0x00}},
        {"candidate 17", XiaomiMessageType::kPhoneRequest, 0x08, {0x01, 0x01}},
        {"candidate 18", XiaomiMessageType::kPhoneRequest, 0x08, {0x01, 0x02}},
        {"candidate 19", XiaomiMessageType::kPhoneRequest, 0x0A, {0x01, 0x00}},
        {"candidate 20", XiaomiMessageType::kPhoneRequest, 0x0A, {0x01, 0x01}},
        {"candidate 21", XiaomiMessageType::kPhoneRequest, 0x0A, {0x01, 0x02}},
        {"candidate 22", XiaomiMessageType::kPhoneRequest, 0x0B, {0x01, 0x00}},
        {"candidate 23", XiaomiMessageType::kPhoneRequest, 0x0B, {0x01, 0x01}},
        {"candidate 24", XiaomiMessageType::kPhoneRequest, 0x0B, {0x01, 0x02}},
    };
}

bool TryExtractBatteryPercent(const IInspectable& raw_value, std::uint8_t* value) {
    if (value == nullptr || raw_value == nullptr) {
        return false;
    }

    try {
        const auto numeric = winrt::unbox_value<std::uint32_t>(raw_value);
        if (numeric <= 100U) {
            *value = static_cast<std::uint8_t>(numeric);
            return true;
        }
    } catch (const winrt::hresult_error&) {
    }

    try {
        const auto numeric = winrt::unbox_value<std::int32_t>(raw_value);
        if (numeric >= 0 && numeric <= 100) {
            *value = static_cast<std::uint8_t>(numeric);
            return true;
        }
    } catch (const winrt::hresult_error&) {
    }

    try {
        const auto numeric = winrt::unbox_value<std::uint8_t>(raw_value);
        if (numeric <= 100U) {
            *value = static_cast<std::uint8_t>(numeric);
            return true;
        }
    } catch (const winrt::hresult_error&) {
    }

    try {
        const auto text = ToUtf8(winrt::unbox_value<winrt::hstring>(raw_value));
        const auto parsed = ParsePercentFromText(text);
        if (parsed.has_value()) {
            *value = *parsed;
            return true;
        }
    } catch (const winrt::hresult_error&) {
    }

    return false;
}

std::vector<BatteryReading> ReadZmiVendorBatteryHint(const DeviceInformation& endpoint_info) {
    constexpr auto kVendorBatteryHintKey = L"{670245F9-6E25-4179-85C1-981C33B9D3B7} 4";
    constexpr auto kVendorGuidPrefixLower = "{670245f9-6e25-4179-85c1-981c33b9d3b7}";

    auto decode_from_value = [](const IInspectable& value) -> std::vector<BatteryReading> {
        if (value == nullptr) {
            return {};
        }

        try {
            const auto normalized = NormalizeZmiVendorBatteryScalar(static_cast<int>(winrt::unbox_value<std::uint8_t>(value)));
            if (normalized.has_value()) {
                return {BatteryReading{"main", *normalized}};
            }
        } catch (const winrt::hresult_error&) {
        }

        try {
            const auto uint32_value = winrt::unbox_value<std::uint32_t>(value);
            auto decoded = DecodeZmiVendorBatteryReadingsFromUInt32(uint32_value);
            if (!decoded.empty()) {
                return decoded;
            }
        } catch (const winrt::hresult_error&) {
        }

        try {
            const auto int32_value = winrt::unbox_value<std::int32_t>(value);
            if (int32_value > 0) {
                auto decoded = DecodeZmiVendorBatteryReadingsFromUInt32(static_cast<std::uint32_t>(int32_value));
                if (!decoded.empty()) {
                    return decoded;
                }
                const auto normalized = NormalizeZmiVendorBatteryScalar(int32_value);
                if (normalized.has_value()) {
                    return {BatteryReading{"main", *normalized}};
                }
            }
        } catch (const winrt::hresult_error&) {
        }

        if (const auto property_value = value.try_as<IPropertyValue>()) {
            try {
                const auto type = property_value.Type();
                if (type == PropertyType::UInt8Array) {
                    winrt::com_array<std::uint8_t> values;
                    property_value.GetUInt8Array(values);
                    std::vector<std::uint8_t> bytes(values.begin(), values.end());
                    auto decoded = DecodeZmiVendorReadingsFromByteBlob(bytes);
                    if (!decoded.empty()) {
                        return decoded;
                    }
                } else if (type == PropertyType::UInt16Array) {
                    winrt::com_array<std::uint16_t> values;
                    property_value.GetUInt16Array(values);
                    std::vector<int> numbers;
                    numbers.reserve(values.size());
                    for (const auto raw : values) {
                        numbers.push_back(static_cast<int>(raw));
                    }
                    auto decoded = DecodeZmiVendorReadingsFromNumericValues(numbers);
                    if (!decoded.empty()) {
                        return decoded;
                    }
                } else if (type == PropertyType::UInt32Array) {
                    winrt::com_array<std::uint32_t> values;
                    property_value.GetUInt32Array(values);
                    std::vector<BatteryReading> best_decoded;
                    std::vector<int> numbers;
                    numbers.reserve(values.size());
                    for (const auto raw : values) {
                        auto decoded = DecodeZmiVendorBatteryReadingsFromUInt32(raw);
                        if (decoded.size() > best_decoded.size()) {
                            best_decoded = std::move(decoded);
                        }
                        if (raw <= static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
                            numbers.push_back(static_cast<int>(raw));
                        }
                    }
                    if (!best_decoded.empty()) {
                        return best_decoded;
                    }
                    auto decoded = DecodeZmiVendorReadingsFromNumericValues(numbers);
                    if (!decoded.empty()) {
                        return decoded;
                    }
                } else if (type == PropertyType::Int32Array) {
                    winrt::com_array<std::int32_t> values;
                    property_value.GetInt32Array(values);
                    std::vector<int> numbers;
                    numbers.reserve(values.size());
                    for (const auto raw : values) {
                        numbers.push_back(raw);
                    }
                    auto decoded = DecodeZmiVendorReadingsFromNumericValues(numbers);
                    if (!decoded.empty()) {
                        return decoded;
                    }
                } else if (type == PropertyType::StringArray) {
                    winrt::com_array<winrt::hstring> values;
                    property_value.GetStringArray(values);
                    for (const auto& text : values) {
                        auto decoded = DecodeZmiVendorReadingsFromNumericValues(ExtractIntegersFromText(ToUtf8(text)));
                        if (!decoded.empty()) {
                            return decoded;
                        }
                    }
                }
            } catch (const winrt::hresult_error&) {
            }
        }

        try {
            const auto text = ToUtf8(winrt::unbox_value<winrt::hstring>(value));
            auto decoded = DecodeZmiVendorReadingsFromNumericValues(ExtractIntegersFromText(text));
            if (!decoded.empty()) {
                return decoded;
            }
        } catch (const winrt::hresult_error&) {
        }

        return {};
    };

    std::vector<BatteryReading> best_single_value;
    std::unordered_set<std::string> processed_keys;

    IInspectable raw_value = nullptr;
    if (!TryGetPropertyValue(endpoint_info, kVendorBatteryHintKey, &raw_value)) {
        DebugLog("ZMI vendor key #4 was not present in DeviceInformation properties");
    } else {
        auto decoded = decode_from_value(raw_value);
        if (decoded.size() >= 2U) {
            DebugLog("ZMI vendor key #4 decoded entries=" + std::to_string(decoded.size()));
            return decoded;
        }
        if (!decoded.empty()) {
            best_single_value = decoded;
        }
        processed_keys.insert(ToLowerAscii(ToUtf8(winrt::to_hstring(kVendorBatteryHintKey))));
    }

    for (const auto& property : endpoint_info.Properties()) {
        const std::string key = ToLowerAscii(ToUtf8(property.Key()));
        if (key.find(kVendorGuidPrefixLower) == std::string::npos) {
            continue;
        }
        if (!processed_keys.insert(key).second) {
            continue;
        }

        auto decoded = decode_from_value(property.Value());
        if (decoded.size() >= 2U) {
            DebugLog("ZMI vendor key '" + key + "' decoded entries=" + std::to_string(decoded.size()));
            return decoded;
        }
        if (DebugEnabled() && !decoded.empty()) {
            DebugLog("ZMI vendor key '" + key + "' decoded scalar entries=" + std::to_string(decoded.size()));
        }
        if (!decoded.empty() && best_single_value.empty()) {
            best_single_value = decoded;
        }
    }

    if (!best_single_value.empty()) {
        return best_single_value;
    }

    DebugLog("ZMI vendor key type was not supported");
    return {};
}

void AppendZmiVendorHintPropertyRequests(
    const winrt::Windows::Foundation::Collections::IVector<winrt::hstring>& requested_properties) {
    constexpr int kPropertyIdMin = 1;
    constexpr int kPropertyIdMax = 32;
    for (int property_id = kPropertyIdMin; property_id <= kPropertyIdMax; ++property_id) {
        requested_properties.Append(winrt::to_hstring(
            "{670245F9-6E25-4179-85C1-981C33B9D3B7} " + std::to_string(property_id)));
    }
}

void AppendBluetoothVisualHintPropertyRequests(
    const winrt::Windows::Foundation::Collections::IVector<winrt::hstring>& requested_properties) {
    requested_properties.Append(L"System.Devices.Aep.Bluetooth.Le.Appearance");
    requested_properties.Append(L"System.Devices.Aep.Bluetooth.Cod.Major");
    requested_properties.Append(L"System.Devices.Aep.Bluetooth.Cod.Minor");
    requested_properties.Append(kDeviceContainerCategoryProperty);
    requested_properties.Append(kDeviceContainerPrimaryCategoryProperty);
    requested_properties.Append(L"System.Devices.AepContainer.Categories");
    requested_properties.Append(L"System.Devices.Aep.Category");
    requested_properties.Append(L"System.Devices.Category");
}

std::optional<std::uint8_t> ReadBatteryPercentFromEndpointProperties(const DeviceInformation& endpoint_info) {
    constexpr std::array<const wchar_t*, 3> kBatteryProperties = {
        L"System.Devices.BatteryLife",
        L"System.Devices.BatteryPlusCharging",
        L"System.Devices.BatteryPlusChargingText",
    };

    for (const auto* property : kBatteryProperties) {
        IInspectable raw_value = nullptr;
        if (!TryGetPropertyValue(endpoint_info, property, &raw_value)) {
            continue;
        }

        std::uint8_t percent = 0;
        if (TryExtractBatteryPercent(raw_value, &percent)) {
            return percent;
        }
    }

    return std::nullopt;
}

bool IsLikelyBluetoothEndpoint(const DeviceInformation& endpoint_info) {
    const std::string lowered_id = ToLowerAscii(ToUtf8(endpoint_info.Id()));
    if (lowered_id.find("bluetooth") != std::string::npos || lowered_id.find("bthenum") != std::string::npos ||
        lowered_id.find("bthhfenum") != std::string::npos || lowered_id.find("mmdevapi") != std::string::npos) {
        return true;
    }

    std::string protocol_text;
    if (TryGetStringProperty(endpoint_info, L"System.Devices.Aep.ProtocolId", &protocol_text)) {
        const std::string protocol = ToLowerAscii(protocol_text);
        if (protocol.find("e0cbf06c-cd8b-4647-bb8a-263b43f0f974") != std::string::npos ||
            protocol.find("bb7bb05e-5972-42b5-94fc-76eaa7084d49") != std::string::npos) {
            return true;
        }
    }

    return false;
}

bool IsLikelyBluetoothDeviceInfo(const DeviceInformation& device_info) {
    const std::string lowered_id = ToLowerAscii(ToUtf8(device_info.Id()));
    return lowered_id.find("bluetooth") != std::string::npos || lowered_id.find("bthenum") != std::string::npos ||
           lowered_id.find("bthhfenum") != std::string::npos || lowered_id.find("bthledevice") != std::string::npos ||
           lowered_id.find("mmdevapi") != std::string::npos;
}

std::vector<std::uint8_t> ReadBufferBytes(const winrt::Windows::Storage::Streams::IBuffer& buffer) {
    const auto reader = DataReader::FromBuffer(buffer);
    std::vector<std::uint8_t> bytes(reader.UnconsumedBufferLength());
    if (!bytes.empty()) {
        reader.ReadBytes(bytes);
    }
    return bytes;
}

struct ScoredXiaomiSnapshot {
    XiaomiBatterySnapshot snapshot;
    int score = -1;
};

struct AdvertisementSnapshotResult {
    std::unordered_map<std::uint64_t, XiaomiBatterySnapshot> by_address;
    std::unordered_map<std::string, XiaomiBatterySnapshot> by_name;
    std::unordered_map<std::uint16_t, XiaomiBatterySnapshot> by_product_id;
};

std::optional<BluetoothLEDevice> TryOpenBleDeviceByAddress(std::uint64_t address,
                                                           std::chrono::milliseconds timeout);

std::optional<ScoredXiaomiSnapshot> DecodeXiaomiSnapshotFromAdvertisementPayload(
    const std::vector<std::uint8_t>& payload) {
    if (payload.empty()) {
        return std::nullopt;
    }

    std::optional<ScoredXiaomiSnapshot> best_candidate;

    auto score_snapshot = [](const XiaomiBatterySnapshot& snapshot, int base_score) {
        int score = base_score;
        const int presence = XiaomiBatteryPresenceCount(snapshot);
        score += presence * 10;
        if (presence == 3) {
            score += 8;
        }
        if (snapshot.left.has_value() && snapshot.right.has_value() && *snapshot.left != *snapshot.right) {
            score += 5;
        }
        if ((snapshot.left.has_value() && *snapshot.left == 100U) ||
            (snapshot.right.has_value() && *snapshot.right == 100U) ||
            (snapshot.case_level.has_value() && *snapshot.case_level == 100U)) {
            score += 2;
        }
        return score;
    };

    auto consider = [&](const XiaomiBatterySnapshot& snapshot, int base_score) {
        const int presence = XiaomiBatteryPresenceCount(snapshot);
        if (presence < 2) {
            return;
        }
        const int score = score_snapshot(snapshot, base_score);
        if (!best_candidate.has_value() || score > best_candidate->score) {
            best_candidate = ScoredXiaomiSnapshot{snapshot, score};
        }
    };

    if (const auto parsed = ExtractBatterySnapshotFromXiaomiPayload(payload, std::nullopt); parsed.has_value()) {
        consider(*parsed, 64);
    }

    if (const auto parsed = ExtractZmiSerialPatternSnapshot(payload); parsed.has_value()) {
        consider(*parsed, 74);
    }

    for (std::size_t index = 0; index + 4U < payload.size(); ++index) {
        if (payload[index] != 0x04U || payload[index + 1U] > 0x7FU) {
            continue;
        }

        XiaomiBatterySnapshot snapshot;
        snapshot.left = ParseXiaomiBatteryRaw(payload[index + 2U]);
        snapshot.right = ParseXiaomiBatteryRaw(payload[index + 3U]);
        snapshot.case_level = ParseXiaomiBatteryRaw(payload[index + 4U]);
        consider(snapshot, 56);
    }

    for (std::size_t index = 0; index + 2U < payload.size(); ++index) {
        const std::uint8_t left_raw = payload[index];
        const std::uint8_t right_raw = payload[index + 1U];
        const std::uint8_t case_raw = payload[index + 2U];

        const int plausible_raw_count =
            (left_raw <= 100U || left_raw == 0xFFU ? 1 : 0) +
            (right_raw <= 100U || right_raw == 0xFFU ? 1 : 0) +
            (case_raw <= 100U || case_raw == 0xFFU ? 1 : 0);
        if (plausible_raw_count < 2) {
            continue;
        }

        XiaomiBatterySnapshot snapshot;
        snapshot.left = ParseXiaomiBatteryRaw(left_raw);
        snapshot.right = ParseXiaomiBatteryRaw(right_raw);
        snapshot.case_level = ParseXiaomiBatteryRaw(case_raw);
        consider(snapshot, 38);
    }

    if (!best_candidate.has_value() || best_candidate->score < 58) {
        return std::nullopt;
    }

    return best_candidate;
}

std::uint64_t ComposeBluetoothAddressFromBytes(const std::uint8_t* bytes, bool reverse_order) {
    if (bytes == nullptr) {
        return 0ULL;
    }

    std::uint64_t address = 0ULL;
    for (std::size_t index = 0; index < 6U; ++index) {
        const std::size_t source_index = reverse_order ? (5U - index) : index;
        address = (address << 8U) | static_cast<std::uint64_t>(bytes[source_index]);
    }
    return address;
}

std::vector<std::uint64_t> ParseXiaomiAddressAliasesFromPayload(const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint64_t> aliases;
    if (payload.size() < 11U) {
        return aliases;
    }

    // Xiaomi Service Data layout: frame-control(2), product-id(2), counter(1), mac(6, optional), ...
    const std::uint16_t frame_control = static_cast<std::uint16_t>(
        payload[0] | (static_cast<std::uint16_t>(payload[1]) << 8U));
    constexpr std::uint16_t kFrameControlMacIncluded = 0x0010U;
    if ((frame_control & kFrameControlMacIncluded) == 0U) {
        return aliases;
    }

    const auto* mac_bytes = payload.data() + 5U;
    auto append_alias = [&](std::uint64_t address) {
        if (address <= 0xFFFFULL || address == 0ULL || address == 0xFFFFFFFFFFFFULL) {
            return;
        }
        if (std::find(aliases.begin(), aliases.end(), address) != aliases.end()) {
            return;
        }
        aliases.push_back(address);
    };

    // Some Xiaomi payloads expose MAC bytes in direct order, some in reversed order.
    append_alias(ComposeBluetoothAddressFromBytes(mac_bytes, false));
    append_alias(ComposeBluetoothAddressFromBytes(mac_bytes, true));

    return aliases;
}

std::optional<std::uint16_t> ParseXiaomiProductIdFromPayload(const std::vector<std::uint8_t>& payload) {
    if (payload.size() < 4U) {
        return std::nullopt;
    }

    const std::uint16_t frame_control = static_cast<std::uint16_t>(
        payload[0] | (static_cast<std::uint16_t>(payload[1]) << 8U));
    const std::uint16_t product_id = static_cast<std::uint16_t>(
        payload[2] | (static_cast<std::uint16_t>(payload[3]) << 8U));

    if (product_id == 0U || product_id == 0xFFFFU) {
        return std::nullopt;
    }

    // Xiaomi beacons normally set at least one control flag; reject obviously invalid headers.
    if (frame_control == 0U) {
        return std::nullopt;
    }

    return product_id;
}

void StoreBestAdvertisementSnapshot(
    std::unordered_map<std::uint64_t, ScoredXiaomiSnapshot>* snapshots,
    std::uint64_t address, const XiaomiBatterySnapshot& snapshot, int score) {
    if (snapshots == nullptr || address <= 0xFFFFULL) {
        return;
    }

    auto found = snapshots->find(address);
    if (found == snapshots->end() || score > found->second.score) {
        (*snapshots)[address] = ScoredXiaomiSnapshot{snapshot, score};
    }
}

void StoreBestAdvertisementSnapshotByName(
    std::unordered_map<std::string, ScoredXiaomiSnapshot>* snapshots,
    const std::string& normalized_name, const XiaomiBatterySnapshot& snapshot, int score) {
    if (snapshots == nullptr || normalized_name.empty()) {
        return;
    }

    auto found = snapshots->find(normalized_name);
    if (found == snapshots->end() || score > found->second.score) {
        (*snapshots)[normalized_name] = ScoredXiaomiSnapshot{snapshot, score};
    }
}

void StoreBestAdvertisementSnapshotByProductId(
    std::unordered_map<std::uint16_t, ScoredXiaomiSnapshot>* snapshots,
    std::uint16_t product_id, const XiaomiBatterySnapshot& snapshot, int score) {
    if (snapshots == nullptr || product_id == 0U || product_id == 0xFFFFU) {
        return;
    }

    auto found = snapshots->find(product_id);
    if (found == snapshots->end() || score > found->second.score) {
        (*snapshots)[product_id] = ScoredXiaomiSnapshot{snapshot, score};
    }
}

AdvertisementSnapshotResult ScanXiaomiAdvertisementSnapshots(
    std::chrono::milliseconds scan_duration) {
    AdvertisementSnapshotResult snapshots;

    try {
        BluetoothLEAdvertisementWatcher watcher;
        watcher.ScanningMode(BluetoothLEScanningMode::Active);

        std::mutex snapshots_mutex;
        std::unordered_map<std::uint64_t, ScoredXiaomiSnapshot> scored_snapshots;
        std::unordered_map<std::string, ScoredXiaomiSnapshot> scored_name_snapshots;
        std::unordered_map<std::uint16_t, ScoredXiaomiSnapshot> scored_pid_snapshots;

        const auto received_token = watcher.Received(
            [&snapshots_mutex, &scored_snapshots, &scored_name_snapshots, &scored_pid_snapshots](
                const BluetoothLEAdvertisementWatcher&,
                const BluetoothLEAdvertisementReceivedEventArgs& args) {
                const auto bluetooth_address = args.BluetoothAddress();
                if (bluetooth_address <= 0xFFFFULL) {
                    return;
                }

                const auto advertisement = args.Advertisement();
                const std::string local_name = ToLowerAscii(ToUtf8(advertisement.LocalName()));
                for (const auto& manufacturer : advertisement.ManufacturerData()) {
                    const auto company_id = manufacturer.CompanyId();
                    if (company_id != 0x038FU && company_id != 0x2717U) {
                        continue;
                    }
                    const auto payload = ReadBufferBytes(manufacturer.Data());
                    const auto decoded = DecodeXiaomiSnapshotFromAdvertisementPayload(payload);
                    if (!decoded.has_value()) {
                        continue;
                    }
                    const auto product_id = ParseXiaomiProductIdFromPayload(payload);
                    const auto address_aliases = ParseXiaomiAddressAliasesFromPayload(payload);

                    int source_bonus = 18;

                    const int score = decoded->score + source_bonus;
                    std::scoped_lock lock(snapshots_mutex);
                    StoreBestAdvertisementSnapshot(&scored_snapshots, bluetooth_address, decoded->snapshot, score);
                    for (const auto alias : address_aliases) {
                        StoreBestAdvertisementSnapshot(&scored_snapshots, alias, decoded->snapshot, score + 6);
                    }
                    StoreBestAdvertisementSnapshotByName(&scored_name_snapshots, local_name, decoded->snapshot, score);
                    if (product_id.has_value()) {
                        StoreBestAdvertisementSnapshotByProductId(
                            &scored_pid_snapshots, *product_id, decoded->snapshot, score + 8);
                    }
                }

                for (const auto& section : advertisement.DataSections()) {
                    const auto section_bytes = ReadBufferBytes(section.Data());
                    std::vector<std::uint8_t> payload;
                    int source_bonus = 0;
                    std::optional<std::uint16_t> product_id;

                    const auto data_type = section.DataType();
                    if (data_type != 0x16U || section_bytes.size() < 2U) {
                        continue;
                    }
                    const std::uint16_t service_uuid = static_cast<std::uint16_t>(
                        section_bytes[0] | (static_cast<std::uint16_t>(section_bytes[1]) << 8U));
                    if (service_uuid != 0xFD2DU && service_uuid != 0xFE95U) {
                        continue;
                    }
                    source_bonus += 20;
                    payload.assign(section_bytes.begin() + 2, section_bytes.end());
                    product_id = ParseXiaomiProductIdFromPayload(payload);

                    const auto decoded = DecodeXiaomiSnapshotFromAdvertisementPayload(payload);
                    if (!decoded.has_value()) {
                        continue;
                    }
                    const auto address_aliases = ParseXiaomiAddressAliasesFromPayload(payload);

                    const int score = decoded->score + source_bonus;
                    std::scoped_lock lock(snapshots_mutex);
                    StoreBestAdvertisementSnapshot(&scored_snapshots, bluetooth_address, decoded->snapshot, score);
                    for (const auto alias : address_aliases) {
                        StoreBestAdvertisementSnapshot(&scored_snapshots, alias, decoded->snapshot, score + 8);
                    }
                    StoreBestAdvertisementSnapshotByName(&scored_name_snapshots, local_name, decoded->snapshot, score);
                    if (product_id.has_value()) {
                        StoreBestAdvertisementSnapshotByProductId(
                            &scored_pid_snapshots, *product_id, decoded->snapshot, score + 10);
                    }
                }
            });

        watcher.Start();
        std::this_thread::sleep_for(scan_duration);
        watcher.Stop();
        watcher.Received(received_token);

        std::scoped_lock lock(snapshots_mutex);
        snapshots.by_address.reserve(scored_snapshots.size());
        for (const auto& [address, scored] : scored_snapshots) {
            snapshots.by_address[address] = scored.snapshot;
            if (DebugEnabled()) {
                const auto readings = BuildXiaomiBatteryReadings(scored.snapshot);
                DebugLog("BLE advertisement snapshot address=" + std::to_string(address) +
                         " entries=" + std::to_string(readings.size()) +
                         " score=" + std::to_string(scored.score));
            }
        }

        for (const auto& [address, scored] : scored_snapshots) {
            const auto maybe_ble_device =
                TryOpenBleDeviceByAddress(address, std::chrono::milliseconds(1200));
            if (!maybe_ble_device.has_value() || !(*maybe_ble_device)) {
                continue;
            }

            const std::string resolved_name = ToLowerAscii(ToUtf8((*maybe_ble_device).Name()));
            if (resolved_name.empty()) {
                continue;
            }
            StoreBestAdvertisementSnapshotByName(
                &scored_name_snapshots, resolved_name, scored.snapshot, scored.score + 6);
            if (DebugEnabled()) {
                DebugLog("BLE advertisement snapshot resolved name='" + resolved_name +
                         "' from address=" + std::to_string(address));
            }
        }

        snapshots.by_name.reserve(scored_name_snapshots.size());
        for (const auto& [name, scored] : scored_name_snapshots) {
            snapshots.by_name[name] = scored.snapshot;
            if (DebugEnabled()) {
                const auto readings = BuildXiaomiBatteryReadings(scored.snapshot);
                DebugLog("BLE advertisement snapshot name='" + name + "'" +
                         " entries=" + std::to_string(readings.size()) +
                         " score=" + std::to_string(scored.score));
            }
        }
        snapshots.by_product_id.reserve(scored_pid_snapshots.size());
        for (const auto& [product_id, scored] : scored_pid_snapshots) {
            snapshots.by_product_id[product_id] = scored.snapshot;
            if (DebugEnabled()) {
                const auto readings = BuildXiaomiBatteryReadings(scored.snapshot);
                std::ostringstream pid_stream;
                pid_stream << "0x" << std::uppercase << std::hex
                           << std::setw(4) << std::setfill('0') << product_id;
                DebugLog("BLE advertisement snapshot productId=" + pid_stream.str() +
                         " entries=" + std::to_string(readings.size()) +
                         " score=" + std::to_string(scored.score));
            }
        }
    } catch (const winrt::hresult_error& error) {
        DebugLog("BLE advertisement fallback failed: " + DescribeHresultError(error));
    } catch (const std::exception& error) {
        DebugLog(std::string("BLE advertisement fallback failed: ") + error.what());
    }

    return snapshots;
}

std::optional<std::array<std::uint8_t, 3>> FindLikelyBatteryTriplet(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 3) {
        return std::nullopt;
    }

    for (std::size_t index = 0; index + 2 < bytes.size(); ++index) {
        const std::uint8_t first = bytes[index];
        const std::uint8_t second = bytes[index + 1];
        const std::uint8_t third = bytes[index + 2];

        if (first > 100U || second > 100U || third > 100U) {
            continue;
        }

        if (first == 0U && second == 0U && third == 0U) {
            continue;
        }

        return std::array<std::uint8_t, 3>{first, second, third};
    }

    return std::nullopt;
}

bool IsPrintableAscii(std::uint8_t value) {
    return value >= 32U && value <= 126U;
}

bool LooksLikeTextPayload(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return false;
    }

    std::size_t printable_count = 0;
    std::size_t alpha_count = 0;
    for (const auto byte : bytes) {
        if (IsPrintableAscii(byte)) {
            ++printable_count;
            if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')) {
                ++alpha_count;
            }
        }
    }

    const bool mostly_printable = (printable_count * 100U) / bytes.size() >= 80U;
    return mostly_printable && alpha_count > 0;
}

bool IsLikelyStandardServiceForMetadata(const std::string& uuid_lower) {
    return uuid_lower == "{00001800-0000-1000-8000-00805f9b34fb}" ||  // Generic Access
           uuid_lower == "{00001801-0000-1000-8000-00805f9b34fb}" ||  // Generic Attribute
           uuid_lower == "{0000180a-0000-1000-8000-00805f9b34fb}";    // Device Information
}

bool IsLikelyNameCharacteristic(const std::string& uuid_lower) {
    return uuid_lower == "{00002a00-0000-1000-8000-00805f9b34fb}" ||  // Device Name
           uuid_lower == "{00002a29-0000-1000-8000-00805f9b34fb}" ||  // Manufacturer Name String
           uuid_lower == "{00002a24-0000-1000-8000-00805f9b34fb}" ||  // Model Number String
           uuid_lower == "{00002a25-0000-1000-8000-00805f9b34fb}" ||  // Serial Number String
           uuid_lower == "{00002a27-0000-1000-8000-00805f9b34fb}" ||  // Hardware Revision String
           uuid_lower == "{00002a26-0000-1000-8000-00805f9b34fb}" ||  // Firmware Revision String
           uuid_lower == "{00002a28-0000-1000-8000-00805f9b34fb}";    // Software Revision String
}

std::vector<BatteryReading> TryReadVendorTripletBattery(const BluetoothLEDevice& device) {
    std::vector<BatteryReading> readings;

    const auto services_result =
        WaitForAsyncResult(device.GetGattServicesAsync(), std::chrono::milliseconds(1500));
    if (!services_result.has_value() || services_result->Status() != GattCommunicationStatus::Success) {
        return readings;
    }

    int best_score = -1000;
    std::array<std::uint8_t, 3> best_triplet{0, 0, 0};
    bool found_candidate = false;

    for (const auto& service : services_result->Services()) {
        const std::string service_uuid = ToLowerAscii(ToUtf8(winrt::to_hstring(service.Uuid())));
        if (service.Uuid() == GattServiceUuids::Battery() || IsLikelyStandardServiceForMetadata(service_uuid)) {
            continue;
        }

        const auto characteristics_result =
            WaitForAsyncResult(service.GetCharacteristicsAsync(), std::chrono::milliseconds(1200));
        if (!characteristics_result.has_value() ||
            characteristics_result->Status() != GattCommunicationStatus::Success) {
            continue;
        }

        for (const auto& characteristic : characteristics_result->Characteristics()) {
            const auto properties = characteristic.CharacteristicProperties();
            if ((properties & GattCharacteristicProperties::Read) != GattCharacteristicProperties::Read) {
                continue;
            }

            const std::string characteristic_uuid = ToLowerAscii(ToUtf8(winrt::to_hstring(characteristic.Uuid())));
            if (IsLikelyNameCharacteristic(characteristic_uuid)) {
                continue;
            }

            const auto read_result =
                WaitForAsyncResult(characteristic.ReadValueAsync(BluetoothCacheMode::Uncached),
                                   std::chrono::milliseconds(900));
            auto resolved_read_result = read_result;
            if (!resolved_read_result.has_value() ||
                resolved_read_result->Status() != GattCommunicationStatus::Success) {
                resolved_read_result = WaitForAsyncResult(
                    characteristic.ReadValueAsync(BluetoothCacheMode::Cached),
                    std::chrono::milliseconds(900));
            }
            if (!resolved_read_result.has_value() ||
                resolved_read_result->Status() != GattCommunicationStatus::Success) {
                continue;
            }

            const auto bytes = ReadBufferBytes(resolved_read_result->Value());
            if (bytes.empty() || LooksLikeTextPayload(bytes)) {
                continue;
            }

            if (DebugEnabled()) {
                std::string hex_dump;
                hex_dump.reserve(bytes.size() * 3);
                constexpr const char* kHex = "0123456789ABCDEF";
                for (const auto byte : bytes) {
                    hex_dump.push_back(kHex[(byte >> 4) & 0x0F]);
                    hex_dump.push_back(kHex[byte & 0x0F]);
                    hex_dump.push_back(' ');
                }
                DebugLog("Vendor read service=" + service_uuid +
                         " char=" + characteristic_uuid + " len=" +
                         std::to_string(bytes.size()) + " data=" + hex_dump);
            }
            const auto triplet = FindLikelyBatteryTriplet(bytes);
            if (!triplet.has_value()) {
                continue;
            }

            int score = 0;
            if (bytes.size() == 3) {
                score += 50;
            } else if (bytes.size() <= 8) {
                score += 20;
            } else if (bytes.size() >= 32) {
                score -= 10;
            }

            const auto& t = *triplet;
            if (!(t[0] == t[1] && t[1] == t[2])) {
                score += 10;
            }
            if (t[0] == 0U || t[1] == 0U || t[2] == 0U) {
                score -= 5;
            }
            if (t[0] == 100U || t[1] == 100U || t[2] == 100U) {
                score += 5;
            }

            if (DebugEnabled()) {
                DebugLog("Vendor triplet candidate: " + std::to_string((*triplet)[0]) + "," +
                         std::to_string((*triplet)[1]) + "," + std::to_string((*triplet)[2]) +
                         " score=" + std::to_string(score));
            }

            if (score > best_score) {
                best_score = score;
                best_triplet = *triplet;
                found_candidate = true;
            }
        }
    }

    if (!found_candidate || best_score < 20) {
        if (DebugEnabled()) {
            DebugLog("Vendor triplet was not confident enough.");
        }
        return readings;
    }

    readings.push_back(BatteryReading{"left", best_triplet[0]});
    readings.push_back(BatteryReading{"right", best_triplet[1]});
    readings.push_back(BatteryReading{"case", best_triplet[2]});

    return readings;
}

std::optional<BluetoothLEDevice> TryOpenBleDeviceByAddress(std::uint64_t address,
                                                           std::chrono::milliseconds timeout) {
    auto wait_for_operation = [timeout](auto operation) -> std::optional<BluetoothLEDevice> {
        try {
            const auto deadline = std::chrono::steady_clock::now() + timeout;

            while (operation.Status() == AsyncStatus::Started && std::chrono::steady_clock::now() < deadline) {
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
            DebugLog("TryOpenBleDeviceByAddress operation failed: " + DescribeHresultError(error));
            return std::nullopt;
        }
    };

    if (const auto device = wait_for_operation(BluetoothLEDevice::FromBluetoothAddressAsync(address)); device.has_value()) {
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

std::vector<DeviceBatteryInfo> ReadConnectedBluetoothDeviceBatteryFast(std::vector<EndpointCandidate>* tws_candidates) {
    std::vector<DeviceBatteryInfo> entries;
    std::unordered_set<std::uint64_t> seen_addresses;
    std::unordered_set<std::string> seen_device_ids;
    std::unordered_map<std::uint64_t, std::optional<std::uint8_t>> hfp_phone_cache;
    std::unordered_map<std::string, std::optional<std::uint8_t>> controller_battery_cache;

    auto read_phone_hfp_battery_cached = [&](std::uint64_t address) -> std::optional<std::uint8_t> {
        const auto found = hfp_phone_cache.find(address);
        if (found != hfp_phone_cache.end()) {
            return found->second;
        }
        const auto pnp_hint = ReadPhoneHfpBatteryHintFromPnpAddress(address);
        if (pnp_hint.has_value()) {
            hfp_phone_cache.insert_or_assign(address, pnp_hint);
            return pnp_hint;
        }
        const auto read = TryReadGenericClassicHfpBattery(address);
        hfp_phone_cache.insert_or_assign(address, read);
        return read;
    };

    auto read_controller_battery_cached =
        [&](const std::string& device_name, const std::string& device_id) -> std::optional<std::uint8_t> {
        std::string cache_key = NormalizeControllerNameForMatch(device_name);
        if (cache_key.empty()) {
            cache_key = ToLowerAscii(device_id);
        }
        if (const auto found = controller_battery_cache.find(cache_key); found != controller_battery_cache.end()) {
            return found->second;
        }

        const int controller_cache_ttl_ms = ControllerBatteryCacheTtlMs();
        const auto persistent_cache_entry = TryGetControllerBatteryCacheEntry(cache_key);
        if (persistent_cache_entry.has_value() && controller_cache_ttl_ms > 0) {
            const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - persistent_cache_entry->captured_at);
            if (age_ms.count() <= controller_cache_ttl_ms) {
                if (DebugEnabled()) {
                    DebugLog("Controller battery cache hit key='" + cache_key +
                             "' ageMs=" + std::to_string(age_ms.count()));
                }
                controller_battery_cache.insert_or_assign(cache_key, persistent_cache_entry->value);
                return persistent_cache_entry->value;
            }
        }

        const auto read = TryReadControllerBatteryFromGameInput(device_name, device_id);
        if (!read.has_value() && IsLikelySonyDualShockController(device_name, device_name, device_id)) {
            const auto hid_read = TryReadDualShockBatteryFromHid(device_name, device_id);
            if (hid_read.has_value()) {
                controller_battery_cache.insert_or_assign(cache_key, hid_read);
                PutControllerBatteryCacheEntry(cache_key, hid_read);
                return hid_read;
            }
        }
        if (!read.has_value() && persistent_cache_entry.has_value() && persistent_cache_entry->value.has_value()) {
            if (DebugEnabled()) {
                const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - persistent_cache_entry->captured_at);
                DebugLog("Controller battery cache stale fallback key='" + cache_key +
                         "' ageMs=" + std::to_string(age_ms.count()));
            }
            controller_battery_cache.insert_or_assign(cache_key, persistent_cache_entry->value);
            return persistent_cache_entry->value;
        }
        if (DebugEnabled() && !read.has_value()) {
            DebugLog("GameInput controller battery fallback: no battery report for '" + device_name +
                     "' id='" + device_id + "'");
        }
        controller_battery_cache.insert_or_assign(cache_key, read);
        PutControllerBatteryCacheEntry(cache_key, read);
        return read;
    };

    auto requested_properties = winrt::single_threaded_vector<winrt::hstring>();
    requested_properties.Append(L"System.ItemNameDisplay");
    requested_properties.Append(L"System.Devices.Aep.DeviceAddress");
    requested_properties.Append(L"System.Devices.Aep.IsConnected");
    requested_properties.Append(L"System.Devices.BatteryLife");
    requested_properties.Append(L"System.Devices.BatteryPlusCharging");
    requested_properties.Append(L"System.Devices.BatteryPlusChargingText");
    AppendBluetoothVisualHintPropertyRequests(requested_properties);
    AppendZmiVendorHintPropertyRequests(requested_properties);

    const auto query_started_at = std::chrono::steady_clock::now();
    const auto maybe_device_infos = WaitForAsyncResult(
        DeviceInformation::FindAllAsync(BluetoothDevice::GetDeviceSelectorFromConnectionStatus(BluetoothConnectionStatus::Connected),
                                        requested_properties, DeviceInformationKind::Device),
        std::chrono::milliseconds(1800));
    if (!maybe_device_infos.has_value() || !(*maybe_device_infos)) {
        DebugLog("Fast connected-device query failed or timed out.");
        return entries;
    }
    const auto device_infos = *maybe_device_infos;
    if (DebugEnabled()) {
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - query_started_at);
        DebugLog("Fast connected-device query took " + std::to_string(elapsed_ms.count()) + " ms");
    }

    DebugLog("Fast connected-device entries scanned: " + std::to_string(device_infos.Size()));

    for (const auto& device_info : device_infos) {
        std::string device_name = ToUtf8(device_info.Name());
        if (device_name.empty()) {
            TryGetStringProperty(device_info, L"System.ItemNameDisplay", &device_name);
        }
        const std::string device_id = ToUtf8(device_info.Id());
        seen_device_ids.insert(device_id);
        const bool likely_tws = IsLikelyTwsDevice(device_name, device_name, device_id);

        std::optional<std::uint64_t> address;
        std::string address_text;
        if (TryGetStringProperty(device_info, L"System.Devices.Aep.DeviceAddress", &address_text)) {
            address = ParseBluetoothAddress(address_text);
        }
        if (!address.has_value()) {
            address = ParseBluetoothAddressFromDeviceId(device_id);
        }
        if (!address.has_value()) {
            const auto maybe_bt_device =
                WaitForAsyncResult(BluetoothDevice::FromIdAsync(device_info.Id()), std::chrono::milliseconds(700));
            if (maybe_bt_device.has_value() && *maybe_bt_device) {
                try {
                    const auto bt_address = (*maybe_bt_device).BluetoothAddress();
                    if (bt_address > 0xFFFFULL) {
                        address = bt_address;
                    }
                } catch (const winrt::hresult_error&) {
                }
            }
        }

        if (likely_tws && tws_candidates != nullptr && address.has_value() &&
            seen_addresses.insert(*address).second) {
            EndpointCandidate candidate;
            candidate.endpoint_id = device_id;
            candidate.endpoint_name = device_name.empty() ? "Unknown" : device_name;
            candidate.bluetooth_address = *address;
            PopulateBluetoothVisualHintsFromDeviceInfo(device_info, &candidate);
            candidate.from_connected_scan = true;
            candidate.is_connected = true;
            tws_candidates->push_back(std::move(candidate));
        }

        const bool is_zmi_family = IsLikelyZmiPurPods(device_name, device_name, device_id);
        std::vector<BatteryReading> zmi_readings;
        if (is_zmi_family) {
            zmi_readings = ReadZmiVendorBatteryHint(device_info);
            if (zmi_readings.empty() && address.has_value()) {
                zmi_readings = ReadZmiVendorBatteryHintFromPnpAddress(*address);
                if (!zmi_readings.empty()) {
                    DebugLog("Fast connected fallback: ZMI PnP vendor key decoded entries=" +
                             std::to_string(zmi_readings.size()));
                }
            }
            if (!zmi_readings.empty()) {
                DebugLog("Fast connected fallback: ZMI vendor key decoded entries=" +
                         std::to_string(zmi_readings.size()));
            }
        }

        if (!zmi_readings.empty()) {
            for (const auto& reading : zmi_readings) {
                DeviceBatteryInfo entry;
                entry.device_id = device_id;
                entry.device_name = device_name.empty() ? "Unknown" : device_name;
                entry.battery_component = reading.component.empty() ? "main" : reading.component;
                entry.battery_level_percent = reading.percent;
                PopulateBluetoothVisualHintsFromDeviceInfo(device_info, &entry);
                entry.is_connected = true;
                entries.push_back(std::move(entry));
            }
            continue;
        }

        auto battery_percent = ReadBatteryPercentFromEndpointProperties(device_info);
        if (!battery_percent.has_value() &&
            address.has_value() &&
            IsLikelyPhoneDevice(device_name, device_name, device_id)) {
            battery_percent = read_phone_hfp_battery_cached(*address);
        }
        if (!battery_percent.has_value() &&
            IsLikelyGameControllerDevice(device_name, device_name, device_id)) {
            battery_percent = read_controller_battery_cached(device_name, device_id);
        }
        DeviceBatteryInfo entry;
        entry.device_id = device_id;
        entry.device_name = device_name.empty() ? "Unknown" : device_name;
        entry.battery_component = NormalizeComponentHint(device_name);
        if (entry.battery_component.empty()) {
            entry.battery_component = "main";
        }
        entry.battery_level_percent = battery_percent;
        PopulateBluetoothVisualHintsFromDeviceInfo(device_info, &entry);
        entry.is_connected = true;
        entries.push_back(std::move(entry));
    }

    if (tws_candidates != nullptr && tws_candidates->size() < 2U) {
        try {
            const auto maybe_paired_infos = WaitForAsyncResult(
                DeviceInformation::FindAllAsync(BluetoothDevice::GetDeviceSelectorFromPairingState(true),
                                                requested_properties, DeviceInformationKind::Device),
                std::chrono::milliseconds(2200));
            if (!maybe_paired_infos.has_value() || !(*maybe_paired_infos)) {
                DebugLog("Paired-device query failed or timed out.");
                return entries;
            }
            const auto paired_infos = *maybe_paired_infos;
            for (const auto& device_info : paired_infos) {
                std::string device_name = ToUtf8(device_info.Name());
                if (device_name.empty()) {
                    TryGetStringProperty(device_info, L"System.ItemNameDisplay", &device_name);
                }
                const std::string device_id = ToUtf8(device_info.Id());
                if (!seen_device_ids.insert(device_id).second) {
                    continue;
                }

                const bool likely_tws = IsLikelyTwsDevice(device_name, device_name, device_id);
                if (!likely_tws && !IsLikelyZmiPurPods(device_name, device_name, device_id)) {
                    continue;
                }

                bool is_connected = false;
                TryGetBooleanProperty(device_info, L"System.Devices.Aep.IsConnected", &is_connected);

                std::optional<std::uint64_t> address;
                std::string address_text;
                if (TryGetStringProperty(device_info, L"System.Devices.Aep.DeviceAddress", &address_text)) {
                    address = ParseBluetoothAddress(address_text);
                }
                if (!address.has_value()) {
                    address = ParseBluetoothAddressFromDeviceId(device_id);
                }

                if (likely_tws && address.has_value() && seen_addresses.insert(*address).second) {
                    EndpointCandidate candidate;
                    candidate.endpoint_id = device_id;
                    candidate.endpoint_name = device_name.empty() ? "Unknown" : device_name;
                    candidate.bluetooth_address = *address;
                    PopulateBluetoothVisualHintsFromDeviceInfo(device_info, &candidate);
                    candidate.from_connected_scan = false;
                    candidate.is_connected = is_connected;
                    tws_candidates->push_back(std::move(candidate));
                }

                const bool is_zmi_family = IsLikelyZmiPurPods(device_name, device_name, device_id);
                std::vector<BatteryReading> zmi_readings;
                if (is_zmi_family) {
                    zmi_readings = ReadZmiVendorBatteryHint(device_info);
                    if (zmi_readings.empty() && address.has_value()) {
                        zmi_readings = ReadZmiVendorBatteryHintFromPnpAddress(*address);
                    }
                }

                if (!zmi_readings.empty()) {
                    for (const auto& reading : zmi_readings) {
                        DeviceBatteryInfo entry;
                        entry.device_id = device_id;
                        entry.device_name = device_name.empty() ? "Unknown" : device_name;
                        entry.battery_component = reading.component.empty() ? "main" : reading.component;
                        entry.battery_level_percent = reading.percent;
                        PopulateBluetoothVisualHintsFromDeviceInfo(device_info, &entry);
                        entry.is_connected = is_connected;
                        entries.push_back(std::move(entry));
                    }
                    continue;
                }

                auto battery_percent = ReadBatteryPercentFromEndpointProperties(device_info);
                DeviceBatteryInfo entry;
                entry.device_id = device_id;
                entry.device_name = device_name.empty() ? "Unknown" : device_name;
                entry.battery_component = NormalizeComponentHint(device_name);
                if (entry.battery_component.empty()) {
                    entry.battery_component = "main";
                }
                entry.battery_level_percent = battery_percent;
                PopulateBluetoothVisualHintsFromDeviceInfo(device_info, &entry);
                entry.is_connected = is_connected;
                entries.push_back(std::move(entry));
            }
        } catch (const winrt::hresult_error&) {
            // Paired device fallback is best-effort.
        }
    }

    DebugLog("Fast connected battery entries: " + std::to_string(entries.size()));
    if (tws_candidates != nullptr) {
        DebugLog("Fast TWS candidates: " + std::to_string(tws_candidates->size()));
    }
    return entries;
}

std::vector<DeviceBatteryInfo> ReadAssociationEndpointBattery(std::vector<EndpointCandidate>* tws_candidates) {
    std::vector<DeviceBatteryInfo> endpoint_entries;
    std::unordered_set<std::string> seen_tws_addresses;

    auto requested_properties = winrt::single_threaded_vector<winrt::hstring>();
    requested_properties.Append(L"System.Devices.Aep.IsConnected");
    requested_properties.Append(L"System.Devices.Aep.ProtocolId");
    requested_properties.Append(L"System.Devices.Aep.DeviceAddress");
    requested_properties.Append(L"System.Devices.BatteryLife");
    requested_properties.Append(L"System.Devices.BatteryPlusCharging");
    requested_properties.Append(L"System.Devices.BatteryPlusChargingText");
    requested_properties.Append(L"System.ItemNameDisplay");
    AppendBluetoothVisualHintPropertyRequests(requested_properties);
    AppendZmiVendorHintPropertyRequests(requested_properties);

    constexpr auto kEndpointSelector = LR"((System.Devices.Aep.IsPresent:=System.StructuredQueryType.Boolean#True)
AND ((System.Devices.Aep.ProtocolId:="{E0CBF06C-CD8B-4647-BB8A-263B43F0F974}")
OR (System.Devices.Aep.ProtocolId:="{BB7BB05E-5972-42B5-94FC-76EAA7084D49}")))";

    const auto query_started_at = std::chrono::steady_clock::now();
    const auto endpoint_infos_operation =
        DeviceInformation::FindAllAsync(kEndpointSelector, requested_properties, DeviceInformationKind::AssociationEndpoint);
    const auto endpoint_infos_result =
        WaitForAsyncResult(endpoint_infos_operation, std::chrono::milliseconds(3600));
    if (DebugEnabled()) {
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - query_started_at);
        DebugLog("AEP query took " + std::to_string(elapsed_ms.count()) + " ms");
    }

    if (!endpoint_infos_result.has_value()) {
        DebugLog("AEP query timed out, skipping slow fallback.");
        return endpoint_entries;
    }

    const auto endpoint_infos = *endpoint_infos_result;
    DebugLog("AEP entries scanned: " + std::to_string(endpoint_infos.Size()));

    for (const auto& endpoint_info : endpoint_infos) {
        if (!IsLikelyBluetoothEndpoint(endpoint_info)) {
            continue;
        }

        std::string endpoint_name = ToUtf8(endpoint_info.Name());
        if (endpoint_name.empty()) {
            TryGetStringProperty(endpoint_info, L"System.ItemNameDisplay", &endpoint_name);
        }
        if (endpoint_name.empty()) {
            endpoint_name = "Unknown";
        }
        const std::string endpoint_id = ToUtf8(endpoint_info.Id());
        bool endpoint_is_connected = false;
        TryGetBooleanProperty(endpoint_info, L"System.Devices.Aep.IsConnected", &endpoint_is_connected);
        const std::string endpoint_name_lower = ToLowerAscii(endpoint_name);
        const bool interesting = endpoint_name_lower.find("redmi") != std::string::npos ||
                                 endpoint_name_lower.find("buds") != std::string::npos ||
                                 endpoint_name_lower.find("zmi") != std::string::npos ||
                                 endpoint_name_lower.find("purpods") != std::string::npos;
        if (interesting) {
            DebugLog("AEP candidate: name='" + endpoint_name + "' id='" + endpoint_id + "'");
        }

        if (tws_candidates != nullptr && LooksLikeTwsDeviceByName(endpoint_name)) {
            std::optional<std::uint64_t> address;
            std::string address_text;
            if (TryGetStringProperty(endpoint_info, L"System.Devices.Aep.DeviceAddress", &address_text)) {
                address = ParseBluetoothAddress(address_text);
                if (interesting) {
                    DebugLog("AEP DeviceAddress raw text: '" + address_text + "'");
                }
            }

            if (!address.has_value()) {
                std::uint64_t numeric_address = 0;
                if (TryGetUInt64Property(endpoint_info, L"System.Devices.Aep.DeviceAddress", &numeric_address) &&
                    numeric_address > 0xFFFFULL) {
                    address = numeric_address;
                }
            }

            if (!address.has_value()) {
                address = ParseBluetoothAddressFromDeviceId(endpoint_id);
                if (interesting && address.has_value()) {
                    DebugLog("AEP address parsed from device id");
                }
            }

            if (address.has_value()) {
                const std::string address_key = std::to_string(*address);
                if (seen_tws_addresses.insert(address_key).second) {
                    EndpointCandidate candidate;
                    candidate.endpoint_id = endpoint_id;
                    candidate.endpoint_name = endpoint_name;
                    candidate.bluetooth_address = *address;
                    PopulateBluetoothVisualHintsFromDeviceInfo(endpoint_info, &candidate);
                    candidate.from_connected_scan = false;
                    candidate.is_connected = endpoint_is_connected;
                    tws_candidates->push_back(std::move(candidate));
                    if (interesting) {
                        DebugLog("AEP tws candidate address: " + address_key);
                    }
                }
            } else if (interesting) {
                DebugLog("AEP tws candidate has no parseable DeviceAddress");
            }
        }

        const bool is_zmi_family = IsLikelyZmiPurPods(endpoint_name, endpoint_name, endpoint_id);
        std::vector<BatteryReading> zmi_readings;
        if (is_zmi_family) {
            zmi_readings = ReadZmiVendorBatteryHint(endpoint_info);
            if (zmi_readings.empty()) {
                const auto parsed_address = ParseBluetoothAddressFromDeviceId(endpoint_id);
                if (parsed_address.has_value()) {
                    zmi_readings = ReadZmiVendorBatteryHintFromPnpAddress(*parsed_address);
                    if (!zmi_readings.empty()) {
                        DebugLog("AEP fallback: ZMI PnP vendor key decoded entries=" +
                                 std::to_string(zmi_readings.size()));
                    }
                }
            }
            if (!zmi_readings.empty()) {
                DebugLog("AEP fallback: ZMI vendor key decoded entries=" +
                         std::to_string(zmi_readings.size()));
            }
        }

        if (!zmi_readings.empty()) {
            for (const auto& reading : zmi_readings) {
                DeviceBatteryInfo entry;
                entry.device_id = endpoint_id;
                entry.device_name = endpoint_name;
                entry.battery_component = reading.component.empty() ? "main" : reading.component;
                entry.battery_level_percent = reading.percent;
                PopulateBluetoothVisualHintsFromDeviceInfo(endpoint_info, &entry);
                entry.is_connected = endpoint_is_connected;
                endpoint_entries.push_back(std::move(entry));
            }
            continue;
        }

        auto battery_percent = ReadBatteryPercentFromEndpointProperties(endpoint_info);
        if (!battery_percent.has_value()) {
            if (interesting) {
                DebugLog("AEP battery props not found for '" + endpoint_name + "'");
            }
            continue;
        }

        DeviceBatteryInfo entry;
        entry.device_id = endpoint_id;
        entry.device_name = endpoint_name;
        entry.battery_component = NormalizeComponentHint(endpoint_name);
        if (entry.battery_component.empty()) {
            entry.battery_component = "main";
        }
        entry.battery_level_percent = *battery_percent;
        PopulateBluetoothVisualHintsFromDeviceInfo(endpoint_info, &entry);
        entry.is_connected = endpoint_is_connected;
        endpoint_entries.push_back(std::move(entry));
        if (interesting) {
            DebugLog("AEP battery found for '" + endpoint_name + "': " + std::to_string(*battery_percent));
        }
    }

    return endpoint_entries;
}

std::vector<DeviceBatteryInfo> ReadGenericDeviceBattery() {
    std::vector<DeviceBatteryInfo> entries;

    auto requested_properties = winrt::single_threaded_vector<winrt::hstring>();
    requested_properties.Append(L"System.Devices.Aep.IsConnected");
    requested_properties.Append(L"System.Devices.BatteryLife");
    requested_properties.Append(L"System.Devices.BatteryPlusCharging");
    requested_properties.Append(L"System.Devices.BatteryPlusChargingText");
    requested_properties.Append(L"System.ItemNameDisplay");
    AppendBluetoothVisualHintPropertyRequests(requested_properties);
    AppendZmiVendorHintPropertyRequests(requested_properties);

    constexpr auto kDeviceSelector = LR"(System.Devices.IsPresent:=System.StructuredQueryType.Boolean#True)";

    const auto maybe_devices = WaitForAsyncResult(
        DeviceInformation::FindAllAsync(kDeviceSelector, requested_properties, DeviceInformationKind::Device),
        std::chrono::milliseconds(2200));
    if (!maybe_devices.has_value() || !(*maybe_devices)) {
        DebugLog("Generic Device query failed or timed out.");
        return entries;
    }
    const auto devices = *maybe_devices;
    DebugLog("Generic Device entries scanned: " + std::to_string(devices.Size()));

    for (const auto& device : devices) {
        if (!IsLikelyBluetoothDeviceInfo(device)) {
            continue;
        }

        const std::string device_name_probe = ToUtf8(device.Name());
        const std::string device_id = ToUtf8(device.Id());
        const std::string lowered_probe = ToLowerAscii(device_name_probe + " " + device_id);
        const bool interesting = lowered_probe.find("redmi") != std::string::npos ||
                                 lowered_probe.find("buds") != std::string::npos ||
                                 lowered_probe.find("zmi") != std::string::npos ||
                                 lowered_probe.find("purpods") != std::string::npos;

        std::string name = ToUtf8(device.Name());
        if (name.empty()) {
            TryGetStringProperty(device, L"System.ItemNameDisplay", &name);
        }
        if (name.empty()) {
            name = "Unknown";
        }

        bool is_connected = false;
        TryGetBooleanProperty(device, L"System.Devices.Aep.IsConnected", &is_connected);

        const bool is_zmi_family = IsLikelyZmiPurPods(device_name_probe, device_name_probe, device_id);
        std::vector<BatteryReading> zmi_readings;
        if (is_zmi_family) {
            zmi_readings = ReadZmiVendorBatteryHint(device);
            if (zmi_readings.empty()) {
                const auto parsed_address = ParseBluetoothAddressFromDeviceId(device_id);
                if (parsed_address.has_value()) {
                    zmi_readings = ReadZmiVendorBatteryHintFromPnpAddress(*parsed_address);
                    if (!zmi_readings.empty()) {
                        DebugLog("Generic fallback: ZMI PnP vendor key decoded entries=" +
                                 std::to_string(zmi_readings.size()));
                    }
                }
            }
            if (!zmi_readings.empty()) {
                DebugLog("Generic fallback: ZMI vendor key decoded entries=" +
                         std::to_string(zmi_readings.size()));
            }
        }

        if (!zmi_readings.empty()) {
            for (const auto& reading : zmi_readings) {
                DeviceBatteryInfo entry;
                entry.device_id = device_id;
                entry.device_name = name;
                entry.battery_component = reading.component.empty() ? "main" : reading.component;
                entry.battery_level_percent = reading.percent;
                PopulateBluetoothVisualHintsFromDeviceInfo(device, &entry);
                entry.is_connected = is_connected;
                entries.push_back(std::move(entry));
            }
            continue;
        }

        auto battery = ReadBatteryPercentFromEndpointProperties(device);
        if (!battery.has_value()) {
            if (interesting) {
                DebugLog("Generic battery props not found for name='" + device_name_probe + "' id='" + device_id + "'");
            }
            continue;
        }

        DeviceBatteryInfo entry;
        entry.device_id = device_id;
        entry.device_name = name;
        entry.battery_component = NormalizeComponentHint(name);
        if (entry.battery_component.empty()) {
            entry.battery_component = "main";
        }
        entry.battery_level_percent = *battery;
        PopulateBluetoothVisualHintsFromDeviceInfo(device, &entry);
        entry.is_connected = is_connected;
        entries.push_back(std::move(entry));
        if (interesting) {
            DebugLog("Generic battery found: name='" + name + "' battery=" + std::to_string(*battery));
        }
    }

    return entries;
}

template <typename TBluetoothDevice>
std::vector<BatteryReading> ReadBatteryReadings(const TBluetoothDevice& device, bool prefer_tws_labels) {
    std::vector<BatteryReading> readings;

    const auto service_result =
        WaitForAsyncResult(device.GetGattServicesForUuidAsync(GattServiceUuids::Battery()),
                           std::chrono::milliseconds(1500));
    if (!service_result.has_value() || service_result->Status() != GattCommunicationStatus::Success) {
        return readings;
    }

    const auto services = service_result->Services();
    for (const auto& service : services) {
        const auto characteristic_result = WaitForAsyncResult(
            service.GetCharacteristicsForUuidAsync(GattCharacteristicUuids::BatteryLevel()),
            std::chrono::milliseconds(1200));
        if (!characteristic_result.has_value() ||
            characteristic_result->Status() != GattCommunicationStatus::Success) {
            continue;
        }

        const auto characteristics = characteristic_result->Characteristics();
        for (const auto& characteristic : characteristics) {
            const auto read_result =
                WaitForAsyncResult(characteristic.ReadValueAsync(BluetoothCacheMode::Uncached),
                                   std::chrono::milliseconds(900));
            auto resolved_read_result = read_result;
            if (!resolved_read_result.has_value() ||
                resolved_read_result->Status() != GattCommunicationStatus::Success) {
                resolved_read_result = WaitForAsyncResult(
                    characteristic.ReadValueAsync(BluetoothCacheMode::Cached),
                    std::chrono::milliseconds(900));
            }
            if (!resolved_read_result.has_value() ||
                resolved_read_result->Status() != GattCommunicationStatus::Success) {
                continue;
            }

            const auto reader = DataReader::FromBuffer(resolved_read_result->Value());
            if (reader.UnconsumedBufferLength() < 1) {
                continue;
            }

            BatteryReading entry;
            entry.component = NormalizeComponentHint(ToUtf8(characteristic.UserDescription()));
            entry.percent = reader.ReadByte();
            readings.push_back(std::move(entry));
        }
    }

    AssignFallbackComponents(&readings, prefer_tws_labels);

    std::unordered_set<std::string> dedupe_keys;
    std::vector<BatteryReading> deduped;
    deduped.reserve(readings.size());

    for (const auto& reading : readings) {
        const std::string key = reading.component + "|" + std::to_string(reading.percent);
        if (!dedupe_keys.insert(key).second) {
            continue;
        }
        deduped.push_back(reading);
    }

    std::sort(deduped.begin(), deduped.end(), [](const BatteryReading& lhs, const BatteryReading& rhs) {
        const int lhs_weight = ComponentSortWeight(lhs.component);
        const int rhs_weight = ComponentSortWeight(rhs.component);
        if (lhs_weight != rhs_weight) {
            return lhs_weight < rhs_weight;
        }
        return lhs.component < rhs.component;
    });

    return deduped;
}

void AddCandidatesFromSelector(const winrt::hstring& selector, std::vector<DeviceInformation>* candidates,
                               std::unordered_set<std::string>* known_ids) {
    if (candidates == nullptr || known_ids == nullptr) {
        return;
    }

    try {
        auto requested_properties = winrt::single_threaded_vector<winrt::hstring>();
        requested_properties.Append(L"System.ItemNameDisplay");
        requested_properties.Append(L"System.Devices.Aep.DeviceAddress");
        AppendBluetoothVisualHintPropertyRequests(requested_properties);
        const auto maybe_device_infos =
            WaitForAsyncResult(DeviceInformation::FindAllAsync(selector, requested_properties), std::chrono::milliseconds(1800));
        if (!maybe_device_infos.has_value() || !(*maybe_device_infos)) {
            DebugLog("Selector scan failed or timed out.");
            return;
        }
        const auto device_infos = *maybe_device_infos;
        for (const auto& info : device_infos) {
            const auto device_id = ToUtf8(info.Id());
            if (!known_ids->insert(device_id).second) {
                continue;
            }
            candidates->push_back(info);
        }
    } catch (const winrt::hresult_error&) {
        // Ignore selector errors and continue with others.
    }
}

std::vector<DeviceInformation> EnumerateCandidateDevices() {
    std::vector<DeviceInformation> candidates;
    std::unordered_set<std::string> known_ids;

    try {
        AddCandidatesFromSelector(
            BluetoothLEDevice::GetDeviceSelectorFromConnectionStatus(BluetoothConnectionStatus::Connected),
            &candidates, &known_ids);
    } catch (const winrt::hresult_error& error) {
        DebugLog("GetDeviceSelectorFromConnectionStatus failed: " + DescribeHresultError(error));
    }

    try {
        AddCandidatesFromSelector(BluetoothLEDevice::GetDeviceSelectorFromPairingState(true), &candidates, &known_ids);
    } catch (const winrt::hresult_error& error) {
        DebugLog("GetDeviceSelectorFromPairingState failed: " + DescribeHresultError(error));
    }

    return candidates;
}

}  // namespace

std::vector<DeviceBatteryInfo> WinRtBatteryProvider::GetDevicesBattery(const BatteryQueryOptions& options) {
    EnsureApartmentInitialized();
    try {

        std::vector<DeviceBatteryInfo> devices_with_battery;
        std::unordered_set<std::uint64_t> addresses_with_real_battery;
        std::unordered_map<std::uint64_t, XiaomiReadResult> xiaomi_classic_cache;
        std::unordered_map<std::string, std::size_t> known_entries;
        std::unordered_set<std::string> paired_device_ids;
        std::unordered_set<std::uint64_t> paired_addresses;
        bool paired_snapshot_loaded = false;
        auto try_add_entry = [&](DeviceBatteryInfo entry) {
            const auto parsed_address = ParseBluetoothAddressFromDeviceId(entry.device_id);
            if (parsed_address.has_value()) {
                if (!entry.device_mode.has_value()) {
                    entry.device_mode = TryGetXiaomiModeCacheEntry(*parsed_address);
                }
                if (!entry.device_submode.has_value()) {
                    entry.device_submode = TryGetXiaomiSubmodeCacheEntry(*parsed_address);
                }
            }
            const std::string dedupe_key = MakeEntryKey(entry);
            const auto known = known_entries.find(dedupe_key);
            if (known != known_entries.end()) {
                auto& existing = devices_with_battery[known->second];
                existing.is_connected = existing.is_connected || entry.is_connected;
                if (!existing.device_mode.has_value() && entry.device_mode.has_value()) {
                    existing.device_mode = entry.device_mode;
                }
                if (!existing.device_submode.has_value() && entry.device_submode.has_value()) {
                    existing.device_submode = entry.device_submode;
                }
                if (!existing.bluetooth_le_appearance.has_value() && entry.bluetooth_le_appearance.has_value()) {
                    existing.bluetooth_le_appearance = entry.bluetooth_le_appearance;
                }
                if (!existing.bluetooth_cod_major.has_value() && entry.bluetooth_cod_major.has_value()) {
                    existing.bluetooth_cod_major = entry.bluetooth_cod_major;
                }
                if (!existing.bluetooth_cod_minor.has_value() && entry.bluetooth_cod_minor.has_value()) {
                    existing.bluetooth_cod_minor = entry.bluetooth_cod_minor;
                }
                AppendUniqueStrings(&existing.device_categories, entry.device_categories);
                return;
            }
            if (entry.battery_level_percent.has_value() && !entry.is_cached) {
                if (parsed_address.has_value()) {
                    addresses_with_real_battery.insert(*parsed_address);
                }
            }
            known_entries.emplace(dedupe_key, devices_with_battery.size());
            devices_with_battery.push_back(std::move(entry));
        };
        auto read_xiaomi_classic_cached =
            [&](std::uint64_t address,
                bool aggressive_retry,
                bool enable_dynamic_port_scan,
                std::size_t min_tws_components = 1U) -> const XiaomiReadResult& {
            auto found = xiaomi_classic_cache.find(address);
            const bool has_cached_result = found != xiaomi_classic_cache.end();
            const bool cached_is_sufficient =
                has_cached_result &&
                (!found->second.readings.empty()) &&
                (min_tws_components <= 1U ||
                 XiaomiResolvedTwsComponentCount(found->second.readings) >= min_tws_components);
            if (cached_is_sufficient) {
                return found->second;
            }

            XiaomiReadResult read_result;
            auto readings = TryReadXiaomiClassicBattery(address, enable_dynamic_port_scan);
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
                auto retried = TryReadXiaomiClassicBattery(address, enable_dynamic_port_scan);
                if (XiaomiReadingsRichnessScore(retried) > XiaomiReadingsRichnessScore(readings)) {
                    readings = std::move(retried);
                }
            }
            if (aggressive_retry && !is_sufficient(readings)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(260));
                auto retried = TryReadXiaomiClassicBattery(address, enable_dynamic_port_scan);
                if (XiaomiReadingsRichnessScore(retried) > XiaomiReadingsRichnessScore(readings)) {
                    readings = std::move(retried);
                }
            }

            if (!readings.empty()) {
                read_result.readings = readings;
                read_result.from_persistent_cache = false;
                if (HasUsefulXiaomiTwsReadings(readings, 2U)) {
                    PutPersistentXiaomiSnapshot(address, SnapshotFromBatteryReadings(readings));
                }
            }

            const auto persistent_snapshot = GetPersistentXiaomiSnapshot(address);
            if (persistent_snapshot.has_value()) {
                auto persistent_readings = BuildXiaomiBatteryReadings(*persistent_snapshot);
                if (!persistent_readings.empty()) {
                    const bool live_sufficient = is_sufficient(read_result.readings);
                    const bool persistent_sufficient = is_sufficient(persistent_readings);
                    if ((!live_sufficient && persistent_sufficient) ||
                        (read_result.readings.empty() &&
                         XiaomiReadingsRichnessScore(persistent_readings) > XiaomiReadingsRichnessScore(read_result.readings))) {
                        read_result.readings = std::move(persistent_readings);
                        read_result.from_persistent_cache = true;
                        DebugLog("Xiaomi classic fallback: using persisted cache for address=" + std::to_string(address));
                    }
                }
            }

            if (has_cached_result) {
                if (XiaomiReadingsRichnessScore(found->second.readings) > XiaomiReadingsRichnessScore(read_result.readings)) {
                    return found->second;
                }
                found->second = std::move(read_result);
                return found->second;
            }

            auto inserted = xiaomi_classic_cache.emplace(address, std::move(read_result));
            return inserted.first->second;
        };

        AdvertisementSnapshotResult advertisement_snapshot_cache;
        bool advertisement_scan_attempted = false;
        bool advertisement_rescan_attempted = false;
        int advertisement_scan_budget_ms = 0;
        auto read_xiaomi_advertisement_cached =
            [&](std::uint64_t address,
                const std::string& device_name_hint,
                bool prefer_extended_scan = false) -> std::vector<BatteryReading> {
            if (address <= 0xFFFFULL) {
                if (device_name_hint.empty()) {
                    return {};
                }
            }

            int requested_scan_ms = XiaomiAdvertisementScanMs();
            if (prefer_extended_scan) {
                const int observe_ms = ZmiObserveMs();
                if (observe_ms > 0) {
                    requested_scan_ms = std::max(requested_scan_ms, std::min(observe_ms, 20000));
                } else {
                    requested_scan_ms = std::max(requested_scan_ms, 3200);
                }
            }

            const auto cache_has_hit = [&]() {
                if (address > 0xFFFFULL &&
                    advertisement_snapshot_cache.by_address.find(address) != advertisement_snapshot_cache.by_address.end()) {
                    return true;
                }

                if (!device_name_hint.empty()) {
                    const std::string normalized_hint = ToLowerAscii(device_name_hint);
                    if (advertisement_snapshot_cache.by_name.find(normalized_hint) != advertisement_snapshot_cache.by_name.end()) {
                        return true;
                    }
                    for (const auto& [name, snapshot] : advertisement_snapshot_cache.by_name) {
                        if (name.empty()) {
                            continue;
                        }
                        if (name.find(normalized_hint) != std::string::npos ||
                            normalized_hint.find(name) != std::string::npos) {
                            return true;
                        }
                    }
                }

                if (address > 0xFFFFULL) {
                    const auto expected_product_id = ReadBluetoothProductIdFromRegistry(address);
                    if (expected_product_id.has_value() &&
                        advertisement_snapshot_cache.by_product_id.find(*expected_product_id) !=
                            advertisement_snapshot_cache.by_product_id.end()) {
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
                auto scanned_result =
                    ScanXiaomiAdvertisementSnapshots(std::chrono::milliseconds(scan_ms));

                merge_snapshot_map(&advertisement_snapshot_cache.by_address, scanned_result.by_address);
                merge_snapshot_map(&advertisement_snapshot_cache.by_name, scanned_result.by_name);
                merge_snapshot_map(&advertisement_snapshot_cache.by_product_id, scanned_result.by_product_id);

                const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - scan_started_at);
                DebugLog(std::string("BLE advertisement fallback ") + phase_tag + " in " +
                         std::to_string(elapsed_ms.count()) +
                         " ms (requested=" + std::to_string(scan_ms) + ")" +
                         ", candidate addresses=" + std::to_string(advertisement_snapshot_cache.by_address.size()) +
                         " names=" + std::to_string(advertisement_snapshot_cache.by_name.size()));
            };
            auto resolve_readings_from_snapshot_cache = [&]() -> std::vector<BatteryReading> {
                const auto found = advertisement_snapshot_cache.by_address.find(address);
                if (found != advertisement_snapshot_cache.by_address.end()) {
                    auto readings = BuildXiaomiBatteryReadings(found->second);
                    if (DebugEnabled()) {
                        std::string components;
                        for (const auto& reading : readings) {
                            if (!components.empty()) {
                                components += ",";
                            }
                            components += reading.component + ":" + std::to_string(reading.percent);
                        }
                        DebugLog("BLE advertisement fallback hit address=" + std::to_string(address) +
                                 " entries=" + std::to_string(readings.size()) +
                                 (components.empty() ? "" : " values=" + components));
                    }
                    return readings;
                }

                if (!device_name_hint.empty()) {
                    const std::string normalized_hint = ToLowerAscii(device_name_hint);
                    auto by_name = advertisement_snapshot_cache.by_name.find(normalized_hint);
                    if (by_name == advertisement_snapshot_cache.by_name.end()) {
                        for (const auto& [name, snapshot] : advertisement_snapshot_cache.by_name) {
                            if (name.empty()) {
                                continue;
                            }
                            if (name.find(normalized_hint) != std::string::npos ||
                                normalized_hint.find(name) != std::string::npos) {
                                by_name = advertisement_snapshot_cache.by_name.find(name);
                                break;
                            }
                        }
                    }
                    if (by_name != advertisement_snapshot_cache.by_name.end()) {
                        auto readings = BuildXiaomiBatteryReadings(by_name->second);
                        if (DebugEnabled()) {
                            DebugLog("BLE advertisement fallback hit by name='" + normalized_hint +
                                     "' entries=" + std::to_string(readings.size()));
                        }
                        return readings;
                    }
                }

                const auto expected_product_id = ReadBluetoothProductIdFromRegistry(address);
                if (expected_product_id.has_value()) {
                    const auto by_pid = advertisement_snapshot_cache.by_product_id.find(*expected_product_id);
                    if (by_pid != advertisement_snapshot_cache.by_product_id.end()) {
                        auto readings = BuildXiaomiBatteryReadings(by_pid->second);
                        if (DebugEnabled()) {
                            std::ostringstream pid_stream;
                            pid_stream << "0x" << std::uppercase << std::hex
                                       << std::setw(4) << std::setfill('0') << *expected_product_id;
                            DebugLog("BLE advertisement fallback hit by productId=" + pid_stream.str() +
                                     " entries=" + std::to_string(readings.size()));
                        }
                        return readings;
                    }
                }

                return {};
            };

            if (!advertisement_scan_attempted ||
                (!cache_has_hit() && requested_scan_ms > advertisement_scan_budget_ms + 250)) {
                advertisement_scan_attempted = true;
                advertisement_scan_budget_ms = std::max(advertisement_scan_budget_ms, requested_scan_ms);
                scan_and_merge(requested_scan_ms, "scanned");
            }

            auto readings = resolve_readings_from_snapshot_cache();
            if (!readings.empty()) {
                return readings;
            }

            if (prefer_extended_scan &&
                !advertisement_rescan_attempted &&
                !cache_has_hit()) {
                advertisement_rescan_attempted = true;
                const int rescan_ms =
                    requested_scan_ms >= 16000 ? 4200 :
                    (requested_scan_ms >= 10000 ? 3200 :
                     std::clamp(requested_scan_ms + 2200, 2200, 12000));
                advertisement_scan_budget_ms = std::max(advertisement_scan_budget_ms, rescan_ms);
                scan_and_merge(rescan_ms, "rescanned");
                readings = resolve_readings_from_snapshot_cache();
                if (!readings.empty()) {
                    return readings;
                }
            }

            return {};
        };

    const auto device_infos = EnumerateCandidateDevices();
    DebugLog("BLE candidates from selectors: " + std::to_string(device_infos.size()));

    for (const auto& device_info : device_infos) {
        try {
            const std::string candidate_id = ToUtf8(device_info.Id());
            if (DebugEnabled()) {
                DebugLog("BLE candidate begin id='" + candidate_id + "'");
            }

            const auto open_started_at = std::chrono::steady_clock::now();
            const auto maybe_ble_device =
                WaitForAsyncResult(BluetoothLEDevice::FromIdAsync(device_info.Id()), std::chrono::milliseconds(1200));
            if (!maybe_ble_device.has_value() || !(*maybe_ble_device)) {
                if (DebugEnabled()) {
                    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - open_started_at);
                    DebugLog("BLE candidate open failed in " + std::to_string(elapsed_ms.count()) +
                             " ms id='" + candidate_id + "'");
                }
                continue;
            }
            const auto ble_device = *maybe_ble_device;
            if (DebugEnabled()) {
                const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - open_started_at);
                DebugLog("BLE candidate open succeeded in " + std::to_string(elapsed_ms.count()) +
                         " ms id='" + candidate_id + "'");
            }

            std::string device_id = candidate_id;
            std::string device_name = ToUtf8(device_info.Name());
            const std::string ble_name = ToUtf8(ble_device.Name());
            if (device_name.empty()) {
                device_name = ble_name;
            }
            if (device_name.empty()) {
                device_name = "Unknown";
            }

            const bool likely_tws = IsLikelyTwsDevice(device_name, ble_name, device_id);
            const bool likely_xiaomi_tws = likely_tws && IsLikelyXiaomiEarbuds(device_name, ble_name, device_id);
            const bool likely_zmi_family = IsLikelyZmiPurPods(device_name, ble_name, device_id);
            const bool aggressive_xiaomi_retry =
                ShouldAggressiveXiaomiClassicRetry(device_name, ble_name, device_id);
            if (DebugEnabled()) {
                const std::string lowered_probe = ToLowerAscii(device_name + " " + ble_name + " " + device_id);
                if (lowered_probe.find("redmi") != std::string::npos ||
                    lowered_probe.find("buds") != std::string::npos ||
                    lowered_probe.find("zmi") != std::string::npos ||
                    lowered_probe.find("purpods") != std::string::npos) {
                    const auto ble_address = TryGetBluetoothAddress(ble_device);
                    DebugLog("BLE device opened: name='" + device_name + "' bleName='" + ble_name + "' id='" + device_id +
                             "' tws=" + (likely_tws ? "true" : "false") +
                             " xiaomiTws=" + (likely_xiaomi_tws ? "true" : "false") +
                             " address=" + (ble_address.has_value() ? std::to_string(*ble_address) : "n/a"));
                }
            }
            std::vector<BatteryReading> battery_readings;
            if (!likely_xiaomi_tws || likely_zmi_family) {
                const auto read_started_at = std::chrono::steady_clock::now();
                battery_readings = ReadBatteryReadings(ble_device, likely_tws);
                if (DebugEnabled()) {
                    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - read_started_at);
                    DebugLog("BLE candidate battery read took " + std::to_string(elapsed_ms.count()) +
                             " ms, entries=" + std::to_string(battery_readings.size()) +
                             " id='" + device_id + "'");
                }
            } else {
                DebugLog("BLE candidate: Xiaomi-family TWS, skip standard battery service read");
            }
            std::vector<BatteryReading> resolved_readings = battery_readings;
            bool resolved_from_persistent_cache = false;
            std::optional<std::uint64_t> resolved_address_for_fallback = TryGetBluetoothAddress(ble_device);
            if (!resolved_address_for_fallback.has_value()) {
                resolved_address_for_fallback = ParseBluetoothAddressFromDeviceId(device_id);
            }

            if (likely_xiaomi_tws && !HasUsefulXiaomiTwsReadings(resolved_readings)) {
                if (resolved_address_for_fallback.has_value()) {
                    const auto& classic_result =
                        read_xiaomi_classic_cached(*resolved_address_for_fallback,
                                                   aggressive_xiaomi_retry,
                                                   likely_zmi_family,
                                                   2U);
                    if (!classic_result.readings.empty()) {
                        resolved_readings = classic_result.readings;
                        resolved_from_persistent_cache = classic_result.from_persistent_cache;
                    }
                } else {
                    DebugLog("BLE Xiaomi fallback skipped because address could not be resolved for '" + device_name + "'");
                }
            }
            if (likely_tws && resolved_readings.size() <= 1U) {
                const auto vendor_triplet = TryReadVendorTripletBattery(ble_device);
                if (vendor_triplet.size() >= 2U) {
                    resolved_readings = vendor_triplet;
                    resolved_from_persistent_cache = false;
                    DebugLog("BLE vendor triplet fallback accepted for '" + device_name + "'");
                }
            }
            if (likely_xiaomi_tws &&
                !HasUsefulXiaomiTwsReadings(resolved_readings) &&
                resolved_address_for_fallback.has_value()) {
                const auto advertisement_readings =
                    read_xiaomi_advertisement_cached(*resolved_address_for_fallback, device_name, likely_zmi_family);
                if (advertisement_readings.size() >= 2U) {
                    resolved_readings = advertisement_readings;
                    resolved_from_persistent_cache = false;
                    DebugLog("BLE advertisement fallback accepted for '" + device_name + "'");
                }
            }
            if (likely_xiaomi_tws &&
                !likely_zmi_family &&
                !HasUsefulXiaomiTwsReadings(resolved_readings)) {
                const auto late_standard_readings = ReadBatteryReadings(ble_device, likely_tws);
                if (XiaomiReadingsRichnessScore(late_standard_readings) >
                    XiaomiReadingsRichnessScore(resolved_readings)) {
                    resolved_readings = late_standard_readings;
                    resolved_from_persistent_cache = false;
                    if (!resolved_readings.empty()) {
                        DebugLog("BLE Xiaomi fallback: standard battery service yielded " +
                                 std::to_string(resolved_readings.size()) +
                                 " entries for '" + device_name + "'");
                    }
                }
            }
            if (likely_xiaomi_tws &&
                !HasUsefulXiaomiTwsReadings(resolved_readings) &&
                resolved_address_for_fallback.has_value()) {
                const auto persistent_snapshot = GetPersistentXiaomiSnapshot(*resolved_address_for_fallback);
                if (persistent_snapshot.has_value()) {
                    auto persisted_readings = BuildXiaomiBatteryReadings(*persistent_snapshot);
                    if (HasUsefulXiaomiTwsReadings(persisted_readings, 2U) &&
                        XiaomiReadingsRichnessScore(persisted_readings) > XiaomiReadingsRichnessScore(resolved_readings)) {
                        resolved_readings = std::move(persisted_readings);
                        resolved_from_persistent_cache = true;
                        DebugLog("BLE Xiaomi fallback: persisted TWS snapshot accepted for '" + device_name + "'");
                    }
                }
            }
            const bool ble_is_connected =
                ble_device.ConnectionStatus() == BluetoothConnectionStatus::Connected;
            if (!likely_tws && resolved_readings.empty()) {
                const auto endpoint_battery = ReadBatteryPercentFromEndpointProperties(device_info);
                if (endpoint_battery.has_value()) {
                    resolved_readings.push_back(BatteryReading{"main", *endpoint_battery});
                    resolved_from_persistent_cache = false;
                    if (DebugEnabled()) {
                        DebugLog("BLE endpoint property fallback accepted for '" + device_name +
                                 "': " + std::to_string(*endpoint_battery));
                    }
                }
            }

            if (resolved_readings.empty()) {
                if (DebugEnabled()) {
                    const std::string lowered_probe = ToLowerAscii(device_name + " " + ble_name + " " + device_id);
                    if (lowered_probe.find("redmi") != std::string::npos ||
                        lowered_probe.find("buds") != std::string::npos ||
                        lowered_probe.find("zmi") != std::string::npos ||
                        lowered_probe.find("purpods") != std::string::npos) {
                        DebugLog("BLE battery not found for '" + device_name + "'");
                    }
                }

                if (likely_tws || ble_is_connected) {
                    DeviceBatteryInfo unknown_entry;
                    unknown_entry.device_id = device_id;
                    unknown_entry.device_name = device_name;
                    unknown_entry.battery_component = "main";
                    unknown_entry.battery_level_percent = std::nullopt;
                    PopulateBluetoothVisualHintsFromDeviceInfo(device_info, &unknown_entry);
                    unknown_entry.is_connected = ble_is_connected;
                    try_add_entry(std::move(unknown_entry));
                }
                continue;
            }
            if (likely_xiaomi_tws &&
                resolved_address_for_fallback.has_value() &&
                !resolved_from_persistent_cache &&
                HasUsefulXiaomiTwsReadings(resolved_readings, 2U)) {
                PutPersistentXiaomiSnapshot(*resolved_address_for_fallback, SnapshotFromBatteryReadings(resolved_readings));
            }

            for (const auto& battery_reading : resolved_readings) {
                DeviceBatteryInfo entry;
                entry.device_id = device_id;
                entry.device_name = device_name;
                entry.battery_component = battery_reading.component;
                entry.battery_level_percent = battery_reading.percent;
                PopulateBluetoothVisualHintsFromDeviceInfo(device_info, &entry);
                entry.is_cached = resolved_from_persistent_cache;
                entry.is_connected = ble_is_connected;
                try_add_entry(std::move(entry));
            }

            const auto resolved_address = TryGetBluetoothAddress(ble_device);
            if (resolved_address.has_value() && !resolved_readings.empty() && !resolved_from_persistent_cache) {
                addresses_with_real_battery.insert(*resolved_address);
            }
        } catch (const winrt::hresult_error&) {
            // Ignore devices that fail to respond and continue with the next one.
        }
    }

    try {
        std::vector<EndpointCandidate> tws_candidates;
        const auto fast_connected_entries = ReadConnectedBluetoothDeviceBatteryFast(&tws_candidates);

        const bool should_scan_aep = ForceAepScanEnabled() || tws_candidates.size() < 2U;
        if (should_scan_aep) {
            std::vector<EndpointCandidate> aep_tws_candidates;
            const auto endpoint_entries = ReadAssociationEndpointBattery(&aep_tws_candidates);
            DebugLog("AEP battery entries: " + std::to_string(endpoint_entries.size()));
            DebugLog("AEP TWS candidates: " + std::to_string(aep_tws_candidates.size()));

            for (const auto& endpoint_entry : endpoint_entries) {
                try_add_entry(endpoint_entry);
            }

            for (const auto& candidate : aep_tws_candidates) {
                auto existing =
                    std::find_if(tws_candidates.begin(), tws_candidates.end(),
                                 [&candidate](const EndpointCandidate& known_candidate) {
                                     return known_candidate.bluetooth_address == candidate.bluetooth_address;
                                 });
                if (existing == tws_candidates.end()) {
                    tws_candidates.push_back(candidate);
                } else {
                    existing->is_connected = existing->is_connected || candidate.is_connected;
                }
            }
        } else {
            DebugLog("AEP scan skipped because fast candidate scan already found targets.");
        }

        for (const auto& candidate : tws_candidates) {
            if (addresses_with_real_battery.contains(candidate.bluetooth_address)) {
                continue;
            }

            if (options.include_disconnected && !candidate.is_connected) {
                DeviceBatteryInfo offline_entry;
                offline_entry.device_id =
                    candidate.endpoint_id.empty() ? ("BluetoothAddress#" + std::to_string(candidate.bluetooth_address))
                                                  : candidate.endpoint_id;
                offline_entry.device_name = candidate.endpoint_name.empty() ? "Unknown" : candidate.endpoint_name;
                offline_entry.battery_component = "main";
                offline_entry.battery_level_percent = std::nullopt;
                PopulateBluetoothVisualHintsFromEndpointCandidate(candidate, &offline_entry);
                offline_entry.is_connected = false;
                try_add_entry(std::move(offline_entry));
                continue;
            }

            const bool likely_xiaomi_tws = IsLikelyXiaomiEarbuds(candidate.endpoint_name, candidate.endpoint_name, candidate.endpoint_id);
            const bool likely_zmi_family =
                IsLikelyZmiPurPods(candidate.endpoint_name, candidate.endpoint_name, candidate.endpoint_id);
            const bool aggressive_xiaomi_retry =
                candidate.from_connected_scan &&
                !options.include_disconnected &&
                ShouldAggressiveXiaomiClassicRetry(candidate.endpoint_name, candidate.endpoint_name, candidate.endpoint_id);
            std::vector<BatteryReading> partial_classic_readings;
            bool partial_classic_from_cache = false;
            if (likely_xiaomi_tws) {
                const auto& classic_result =
                    read_xiaomi_classic_cached(candidate.bluetooth_address,
                                               aggressive_xiaomi_retry,
                                               likely_zmi_family,
                                               2U);
                if (!classic_result.readings.empty()) {
                    if (!HasUsefulXiaomiTwsReadings(classic_result.readings)) {
                        partial_classic_readings = classic_result.readings;
                        partial_classic_from_cache = classic_result.from_persistent_cache;
                        DebugLog("AEP Xiaomi classic result is partial for '" + candidate.endpoint_name +
                                 "', continue BLE/vendor fallbacks");
                    } else {
                    const std::string device_id =
                        candidate.endpoint_id.empty() ? ("BluetoothAddress#" + std::to_string(candidate.bluetooth_address))
                                                      : candidate.endpoint_id;
                    const std::string device_name = candidate.endpoint_name.empty() ? "Unknown" : candidate.endpoint_name;

                    for (const auto& battery_reading : classic_result.readings) {
                        DeviceBatteryInfo entry;
                        entry.device_id = device_id;
                        entry.device_name = device_name;
                        entry.battery_component = battery_reading.component;
                        entry.battery_level_percent = battery_reading.percent;
                        entry.is_cached = classic_result.from_persistent_cache;
                        entry.is_connected = candidate.is_connected;
                        try_add_entry(std::move(entry));
                    }
                    continue;
                    }
                }
            }

            const auto maybe_ble_device =
                TryOpenBleDeviceByAddress(candidate.bluetooth_address, std::chrono::milliseconds(1200));
            if (!maybe_ble_device.has_value()) {
                if (DebugEnabled()) {
                    const std::string lowered_probe = ToLowerAscii(candidate.endpoint_name + " " + candidate.endpoint_id);
                    if (lowered_probe.find("redmi") != std::string::npos ||
                        lowered_probe.find("buds") != std::string::npos ||
                        lowered_probe.find("zmi") != std::string::npos ||
                        lowered_probe.find("purpods") != std::string::npos) {
                        DebugLog("AEP TWS address open failed for '" + candidate.endpoint_name + "' address=" +
                                 std::to_string(candidate.bluetooth_address));
                    }
                }

                if (likely_xiaomi_tws) {
                    const auto advertisement_readings =
                        read_xiaomi_advertisement_cached(candidate.bluetooth_address,
                                                         candidate.endpoint_name,
                                                         likely_zmi_family);
                    if (advertisement_readings.size() >= 2U) {
                        PutPersistentXiaomiSnapshot(candidate.bluetooth_address,
                                                    SnapshotFromBatteryReadings(advertisement_readings));
                        const std::string device_id =
                            candidate.endpoint_id.empty() ? ("BluetoothAddress#" + std::to_string(candidate.bluetooth_address))
                                                          : candidate.endpoint_id;
                        const std::string device_name = candidate.endpoint_name.empty() ? "Unknown" : candidate.endpoint_name;

                        for (const auto& battery_reading : advertisement_readings) {
                            DeviceBatteryInfo entry;
                            entry.device_id = device_id;
                            entry.device_name = device_name;
                            entry.battery_component = battery_reading.component;
                            entry.battery_level_percent = battery_reading.percent;
                            entry.is_connected = candidate.is_connected;
                            try_add_entry(std::move(entry));
                        }
                        DebugLog("AEP advertisement fallback used without BLE open for '" + device_name + "'");
                        continue;
                    }
                }

                if (!partial_classic_readings.empty()) {
                    const std::string device_id =
                        candidate.endpoint_id.empty() ? ("BluetoothAddress#" + std::to_string(candidate.bluetooth_address))
                                                      : candidate.endpoint_id;
                    const std::string device_name = candidate.endpoint_name.empty() ? "Unknown" : candidate.endpoint_name;
                    for (const auto& battery_reading : partial_classic_readings) {
                        DeviceBatteryInfo entry;
                        entry.device_id = device_id;
                        entry.device_name = device_name;
                        entry.battery_component = battery_reading.component;
                        entry.battery_level_percent = battery_reading.percent;
                        entry.is_cached = partial_classic_from_cache;
                        entry.is_connected = candidate.is_connected;
                        try_add_entry(std::move(entry));
                    }
                    continue;
                }
                if (likely_xiaomi_tws) {
                    const auto persistent_snapshot = GetPersistentXiaomiSnapshot(candidate.bluetooth_address);
                    if (persistent_snapshot.has_value()) {
                        auto persisted_readings = BuildXiaomiBatteryReadings(*persistent_snapshot);
                        if (HasUsefulXiaomiTwsReadings(persisted_readings, 2U)) {
                            const std::string device_id =
                                candidate.endpoint_id.empty() ? ("BluetoothAddress#" + std::to_string(candidate.bluetooth_address))
                                                              : candidate.endpoint_id;
                            const std::string device_name = candidate.endpoint_name.empty() ? "Unknown" : candidate.endpoint_name;
                            for (const auto& battery_reading : persisted_readings) {
                                DeviceBatteryInfo entry;
                                entry.device_id = device_id;
                                entry.device_name = device_name;
                                entry.battery_component = battery_reading.component;
                                entry.battery_level_percent = battery_reading.percent;
                                entry.is_cached = true;
                                entry.is_connected = candidate.is_connected;
                                try_add_entry(std::move(entry));
                            }
                            DebugLog("AEP fallback: persisted TWS snapshot used for '" + device_name + "'");
                            continue;
                        }
                    }
                }

                DeviceBatteryInfo unknown_entry;
                unknown_entry.device_id =
                    candidate.endpoint_id.empty() ? ("BluetoothAddress#" + std::to_string(candidate.bluetooth_address))
                                                  : candidate.endpoint_id;
                unknown_entry.device_name = candidate.endpoint_name.empty() ? "Unknown" : candidate.endpoint_name;
                unknown_entry.battery_component = "main";
                unknown_entry.battery_level_percent = std::nullopt;
                PopulateBluetoothVisualHintsFromEndpointCandidate(candidate, &unknown_entry);
                unknown_entry.is_connected = candidate.is_connected;
                try_add_entry(std::move(unknown_entry));
                continue;
            }
            const auto ble_device = *maybe_ble_device;
            const bool candidate_ble_connected =
                ble_device.ConnectionStatus() == BluetoothConnectionStatus::Connected;

            std::vector<BatteryReading> resolved_readings;
            if (!likely_xiaomi_tws || likely_zmi_family) {
                resolved_readings = ReadBatteryReadings(ble_device, true);
            } else {
                DebugLog("AEP candidate: Xiaomi-family TWS, skip standard battery service read");
            }
            bool resolved_from_persistent_cache = false;
            if (likely_xiaomi_tws && !HasUsefulXiaomiTwsReadings(resolved_readings)) {
                const auto& classic_result =
                    read_xiaomi_classic_cached(candidate.bluetooth_address,
                                               aggressive_xiaomi_retry,
                                               likely_zmi_family,
                                               2U);
                if (!classic_result.readings.empty()) {
                    resolved_readings = classic_result.readings;
                    resolved_from_persistent_cache = classic_result.from_persistent_cache;
                }
            }
            if (resolved_readings.size() <= 1U) {
                const auto vendor_triplet = TryReadVendorTripletBattery(ble_device);
                if (vendor_triplet.size() >= 2U) {
                    resolved_readings = vendor_triplet;
                    resolved_from_persistent_cache = false;
                    DebugLog("AEP vendor triplet fallback accepted for '" + candidate.endpoint_name + "'");
                }
            }
            if (likely_xiaomi_tws && !HasUsefulXiaomiTwsReadings(resolved_readings)) {
                const auto advertisement_readings =
                    read_xiaomi_advertisement_cached(candidate.bluetooth_address,
                                                     candidate.endpoint_name,
                                                     likely_zmi_family);
                if (advertisement_readings.size() >= 2U) {
                    resolved_readings = advertisement_readings;
                    resolved_from_persistent_cache = false;
                    DebugLog("AEP advertisement fallback accepted for '" + candidate.endpoint_name + "'");
                }
            }
            if (likely_xiaomi_tws &&
                !HasUsefulXiaomiTwsReadings(resolved_readings) &&
                !partial_classic_readings.empty() &&
                XiaomiReadingsRichnessScore(partial_classic_readings) > XiaomiReadingsRichnessScore(resolved_readings)) {
                resolved_readings = partial_classic_readings;
                resolved_from_persistent_cache = partial_classic_from_cache;
            }
            if (likely_xiaomi_tws &&
                !likely_zmi_family &&
                !HasUsefulXiaomiTwsReadings(resolved_readings)) {
                const auto late_standard_readings = ReadBatteryReadings(ble_device, true);
                if (XiaomiReadingsRichnessScore(late_standard_readings) >
                    XiaomiReadingsRichnessScore(resolved_readings)) {
                    resolved_readings = late_standard_readings;
                    resolved_from_persistent_cache = false;
                    if (!resolved_readings.empty()) {
                        DebugLog("AEP Xiaomi fallback: standard battery service yielded " +
                                 std::to_string(resolved_readings.size()) +
                                 " entries for '" + candidate.endpoint_name + "'");
                    }
                }
            }
            if (likely_xiaomi_tws && !HasUsefulXiaomiTwsReadings(resolved_readings)) {
                const auto persistent_snapshot = GetPersistentXiaomiSnapshot(candidate.bluetooth_address);
                if (persistent_snapshot.has_value()) {
                    auto persisted_readings = BuildXiaomiBatteryReadings(*persistent_snapshot);
                    if (HasUsefulXiaomiTwsReadings(persisted_readings, 2U) &&
                        XiaomiReadingsRichnessScore(persisted_readings) > XiaomiReadingsRichnessScore(resolved_readings)) {
                        resolved_readings = std::move(persisted_readings);
                        resolved_from_persistent_cache = true;
                        DebugLog("AEP Xiaomi fallback: persisted TWS snapshot accepted for '" +
                                 candidate.endpoint_name + "'");
                    }
                }
            }
            if (resolved_readings.empty()) {
                if (DebugEnabled()) {
                    const std::string lowered_probe = ToLowerAscii(candidate.endpoint_name + " " + candidate.endpoint_id);
                    if (lowered_probe.find("redmi") != std::string::npos ||
                        lowered_probe.find("buds") != std::string::npos ||
                        lowered_probe.find("zmi") != std::string::npos ||
                        lowered_probe.find("purpods") != std::string::npos) {
                        DebugLog("AEP TWS BLE battery not found for '" + candidate.endpoint_name + "'");
                    }
                }

                DeviceBatteryInfo unknown_entry;
                unknown_entry.device_id =
                    candidate.endpoint_id.empty() ? ("BluetoothAddress#" + std::to_string(candidate.bluetooth_address))
                                                  : candidate.endpoint_id;
                unknown_entry.device_name = candidate.endpoint_name.empty() ? "Unknown" : candidate.endpoint_name;
                unknown_entry.battery_component = "main";
                unknown_entry.battery_level_percent = std::nullopt;
                PopulateBluetoothVisualHintsFromEndpointCandidate(candidate, &unknown_entry);
                unknown_entry.is_connected = candidate_ble_connected;
                try_add_entry(std::move(unknown_entry));
                continue;
            }
            if (likely_xiaomi_tws &&
                !resolved_from_persistent_cache &&
                HasUsefulXiaomiTwsReadings(resolved_readings, 2U)) {
                PutPersistentXiaomiSnapshot(candidate.bluetooth_address, SnapshotFromBatteryReadings(resolved_readings));
            }

            std::string device_name = candidate.endpoint_name;
            if (device_name.empty()) {
                device_name = ToUtf8(ble_device.Name());
            }
            if (device_name.empty()) {
                device_name = "Unknown";
            }

            const std::string device_id =
                candidate.endpoint_id.empty() ? ("BluetoothAddress#" + std::to_string(candidate.bluetooth_address))
                                              : candidate.endpoint_id;

            for (const auto& battery_reading : resolved_readings) {
                DeviceBatteryInfo entry;
                entry.device_id = device_id;
                entry.device_name = device_name;
                entry.battery_component = battery_reading.component;
                entry.battery_level_percent = battery_reading.percent;
                PopulateBluetoothVisualHintsFromEndpointCandidate(candidate, &entry);
                entry.is_cached = resolved_from_persistent_cache;
                entry.is_connected = candidate_ble_connected;
                try_add_entry(std::move(entry));
            }
        }

        for (const auto& fast_entry : fast_connected_entries) {
            try_add_entry(fast_entry);
        }
    } catch (const winrt::hresult_error&) {
        // Endpoint properties and endpoint BLE mapping are optional.
    }

    const bool run_generic_scan = GenericScanEnabled() || devices_with_battery.empty();
    if (run_generic_scan) {
        try {
            const auto generic_entries = ReadGenericDeviceBattery();
            DebugLog("Generic battery entries: " + std::to_string(generic_entries.size()));
            for (const auto& generic_entry : generic_entries) {
                try_add_entry(generic_entry);
            }
        } catch (const winrt::hresult_error&) {
            // Generic device battery properties may be unavailable.
        }
    } else {
        DebugLog("Generic device scan skipped (set BATTERY_MONITOR_GENERIC_SCAN=1 to enable).");
    }

    if (options.include_disconnected) {
        try {
            auto requested_properties = winrt::single_threaded_vector<winrt::hstring>();
            requested_properties.Append(L"System.ItemNameDisplay");
            requested_properties.Append(L"System.Devices.Aep.DeviceAddress");
            requested_properties.Append(L"System.Devices.Aep.IsConnected");
            AppendBluetoothVisualHintPropertyRequests(requested_properties);

            std::unordered_set<std::string> processed_paired_ids;
            auto collect_paired = [&](const winrt::hstring& selector) {
                const auto maybe_paired_infos = WaitForAsyncResult(
                    DeviceInformation::FindAllAsync(selector, requested_properties, DeviceInformationKind::Device),
                    std::chrono::milliseconds(2200));
                if (!maybe_paired_infos.has_value() || !(*maybe_paired_infos)) {
                    return;
                }
                paired_snapshot_loaded = true;
                const auto paired_infos = *maybe_paired_infos;
                for (const auto& device_info : paired_infos) {
                    if (!IsLikelyBluetoothDeviceInfo(device_info)) {
                        continue;
                    }

                    const std::string device_id = ToUtf8(device_info.Id());
                    if (device_id.empty()) {
                        continue;
                    }
                    if (!processed_paired_ids.insert(device_id).second) {
                        continue;
                    }

                    std::string device_name = ToUtf8(device_info.Name());
                    if (device_name.empty()) {
                        TryGetStringProperty(device_info, L"System.ItemNameDisplay", &device_name);
                    }
                    if (device_name.empty()) {
                        device_name = "Unknown";
                    }

                    bool is_connected = false;
                    TryGetBooleanProperty(device_info, L"System.Devices.Aep.IsConnected", &is_connected);

                    std::optional<std::uint64_t> address;
                    std::string address_text;
                    if (TryGetStringProperty(device_info, L"System.Devices.Aep.DeviceAddress", &address_text)) {
                        address = ParseBluetoothAddress(address_text);
                    }
                    if (!address.has_value()) {
                        address = ParseBluetoothAddressFromDeviceId(device_id);
                    }
                    paired_device_ids.insert(device_id);
                    if (address.has_value()) {
                        paired_addresses.insert(*address);
                    }
                    if (is_connected) {
                        continue;
                    }

                    const bool already_known = std::any_of(
                        devices_with_battery.begin(), devices_with_battery.end(),
                        [&](const DeviceBatteryInfo& existing) {
                            if (existing.device_id == device_id) {
                                return true;
                            }
                            if (!address.has_value()) {
                                return false;
                            }
                            const auto existing_address = ParseBluetoothAddressFromDeviceId(existing.device_id);
                            return existing_address.has_value() && *existing_address == *address;
                        });
                    if (already_known) {
                        continue;
                    }

                    DeviceBatteryInfo entry;
                    entry.device_id = device_id;
                    entry.device_name = device_name;
                    entry.battery_component = NormalizeComponentHint(device_name);
                    if (entry.battery_component.empty()) {
                        entry.battery_component = "main";
                    }
                    entry.is_connected = false;
                    try_add_entry(std::move(entry));
                }
            };

            collect_paired(BluetoothDevice::GetDeviceSelectorFromPairingState(true));
            collect_paired(BluetoothLEDevice::GetDeviceSelectorFromPairingState(true));
        } catch (const winrt::hresult_error&) {
            // Paired fallback for disconnected devices is best-effort.
        }
    }

    std::unordered_set<std::string> devices_with_real_battery;
    std::unordered_set<std::string> devices_with_live_battery;
    std::unordered_set<std::uint64_t> addresses_with_live_battery;
    std::unordered_set<std::uint64_t> addresses_with_any_real_battery;
    std::unordered_set<std::string> devices_with_tws_components;
    std::unordered_set<std::uint64_t> addresses_with_tws_components;
    for (const auto& entry : devices_with_battery) {
        if (entry.battery_level_percent.has_value()) {
            devices_with_real_battery.insert(entry.device_id);
            const auto parsed_address = ParseBluetoothAddressFromDeviceId(entry.device_id);
            if (parsed_address.has_value()) {
                addresses_with_any_real_battery.insert(*parsed_address);
            }
            if (!entry.is_cached) {
                devices_with_live_battery.insert(entry.device_id);
                if (parsed_address.has_value()) {
                    addresses_with_live_battery.insert(*parsed_address);
                }
            }
        }

        if (entry.battery_level_percent.has_value()) {
            const std::string component = ToLowerAscii(entry.battery_component);
            if (component == "left" || component == "right" || component == "case") {
                const auto parsed_address = ParseBluetoothAddressFromDeviceId(entry.device_id);
                if (parsed_address.has_value()) {
                    addresses_with_tws_components.insert(*parsed_address);
                } else {
                    devices_with_tws_components.insert(entry.device_id);
                }
            }
        }
    }

    std::unordered_map<std::uint64_t, std::optional<PnpBluetoothVisualHints>> pnp_visual_hint_cache;
    for (auto& entry : devices_with_battery) {
        const bool needs_pnp_visual_hints =
            !entry.bluetooth_cod_major.has_value() && !entry.bluetooth_cod_minor.has_value() &&
            entry.device_categories.empty();
        if (!needs_pnp_visual_hints) {
            continue;
        }

        const auto parsed_address = ParseBluetoothAddressFromDeviceId(entry.device_id);
        if (!parsed_address.has_value()) {
            continue;
        }

        auto cache_it = pnp_visual_hint_cache.find(*parsed_address);
        if (cache_it == pnp_visual_hint_cache.end()) {
            cache_it = pnp_visual_hint_cache.emplace(
                *parsed_address, ReadBluetoothVisualHintsFromPnpAddress(*parsed_address)).first;
        }
        if (!cache_it->second.has_value()) {
            continue;
        }

        const auto& hints = *cache_it->second;
        if (!entry.bluetooth_cod_major.has_value() && hints.bluetooth_cod_major.has_value()) {
            entry.bluetooth_cod_major = hints.bluetooth_cod_major;
        }
        if (!entry.bluetooth_cod_minor.has_value() && hints.bluetooth_cod_minor.has_value()) {
            entry.bluetooth_cod_minor = hints.bluetooth_cod_minor;
        }
        AppendUniqueStrings(&entry.device_categories, hints.device_categories);
    }

    std::vector<DeviceBatteryInfo> filtered_entries;
    filtered_entries.reserve(devices_with_battery.size());
    std::unordered_map<std::string, std::size_t> final_dedup;
    for (auto& entry : devices_with_battery) {
        const auto parsed_address = ParseBluetoothAddressFromDeviceId(entry.device_id);
        const std::string normalized_component = ToLowerAscii(entry.battery_component);

        if (normalized_component == "main" &&
            (devices_with_tws_components.contains(entry.device_id) ||
             (parsed_address.has_value() && addresses_with_tws_components.contains(*parsed_address)))) {
            continue;
        }

        if (!entry.battery_level_percent.has_value() &&
            (devices_with_real_battery.contains(entry.device_id) ||
             (parsed_address.has_value() && addresses_with_any_real_battery.contains(*parsed_address)))) {
            continue;
        }
        if (entry.is_cached &&
            (devices_with_live_battery.contains(entry.device_id) ||
             (parsed_address.has_value() && addresses_with_live_battery.contains(*parsed_address)))) {
            continue;
        }

        std::string key;
        if (!entry.battery_level_percent.has_value()) {
            if (parsed_address.has_value()) {
                key = "unknown|" + std::to_string(*parsed_address) + "|" + entry.battery_component;
            } else {
                key = "unknown|" + entry.device_id + "|" + entry.battery_component;
            }
        } else {
            if (parsed_address.has_value()) {
                key = "resolved|" + std::to_string(*parsed_address) + "|" + entry.battery_component +
                      "|" + (entry.is_cached ? "cached" : "live");
            } else {
                key = "resolved|" + entry.device_id + "|" + entry.battery_component +
                      "|" + (entry.is_cached ? "cached" : "live");
            }
        }
        const auto dedup_it = final_dedup.find(key);
        if (dedup_it != final_dedup.end()) {
            auto& existing = filtered_entries[dedup_it->second];
            existing.is_connected = existing.is_connected || entry.is_connected;
            continue;
        }

        final_dedup.emplace(key, filtered_entries.size());
        filtered_entries.push_back(std::move(entry));
    }

    if (options.include_disconnected && paired_snapshot_loaded) {
        filtered_entries.erase(
            std::remove_if(filtered_entries.begin(), filtered_entries.end(),
                           [&](const DeviceBatteryInfo& entry) {
                               if (paired_device_ids.contains(entry.device_id)) {
                                   return false;
                               }
                               const auto parsed_address = ParseBluetoothAddressFromDeviceId(entry.device_id);
                               return !parsed_address.has_value() || !paired_addresses.contains(*parsed_address);
                           }),
            filtered_entries.end());
    }

    if (!options.include_disconnected) {
        filtered_entries.erase(
            std::remove_if(filtered_entries.begin(), filtered_entries.end(),
                           [](const DeviceBatteryInfo& entry) {
                               return !entry.is_connected;
                           }),
            filtered_entries.end());
    }

    return filtered_entries;
    } catch (const winrt::hresult_error& error) {
        DebugLog("GetConnectedDevicesBattery failed with WinRT error: " + DescribeHresultError(error));
        return {};
    } catch (const std::exception& error) {
        DebugLog(std::string("GetConnectedDevicesBattery failed: ") + error.what());
        return {};
    } catch (...) {
        DebugLog("GetConnectedDevicesBattery failed with unknown exception.");
        return {};
    }
}

bool WinRtBatteryProvider::ProbeXiaomiNoiseControl(const std::string& device_hint) {
    EnsureApartmentInitialized();

    BatteryQueryOptions options;
    options.include_disconnected = false;
    const auto devices = GetDevicesBattery(options);

    std::vector<std::pair<std::string, std::uint64_t>> candidates;
    std::unordered_set<std::uint64_t> seen_addresses;
    const std::string normalized_hint = ToLowerAscii(device_hint);
    for (const auto& entry : devices) {
        if (!entry.is_connected) {
            continue;
        }
        if (!IsLikelyXiaomiEarbuds(entry.device_name, entry.device_name, entry.device_id)) {
            continue;
        }
        if (!normalized_hint.empty()) {
            const std::string probe = ToLowerAscii(entry.device_name + " " + entry.device_id);
            if (probe.find(normalized_hint) == std::string::npos) {
                continue;
            }
        }

        const auto address = ParseBluetoothAddressFromDeviceId(entry.device_id);
        if (!address.has_value() || !seen_addresses.insert(*address).second) {
            continue;
        }
        candidates.emplace_back(entry.device_name, *address);
    }

    if (candidates.empty()) {
        std::cout << "No connected Xiaomi/Redmi earbuds candidates were found.\n";
        return false;
    }

    const auto& target = candidates.front();
    std::cout << "Probing device: " << target.first << " address=" << target.second << "\n";
    std::cout << "Watch the earbuds state. Each candidate waits about 2 seconds.\n";
    std::cout << "If a mode changes, note the candidate number and the observed mode.\n";

    ScopedWsa wsa;
    if (!wsa.started()) {
        std::cout << "WSAStartup failed.\n";
        return false;
    }

    SOCKET socket_handle = INVALID_SOCKET;
    std::string connected_path;
    if (!ConnectXiaomiControlSocket(target.second, &socket_handle, &connected_path)) {
        std::cout << "Failed to open Xiaomi control socket.\n";
        return false;
    }

    std::cout << "Connected via " << connected_path << "\n";

    std::uint8_t sequence = 0;
    const bool auth_ok = RunXiaomiAuthHandshake(socket_handle, &sequence);
    if (!auth_ok) {
        std::cout << "Auth handshake failed.\n";
        closesocket(socket_handle);
        return false;
    }

    const auto commands = BuildXiaomiNoiseProbeCommands();
    std::vector<std::uint8_t> rx_buffer;
    for (std::size_t index = 0; index < commands.size(); ++index) {
        const auto& command = commands[index];
        XiaomiMessage message;
        message.type = command.type;
        message.opcode = static_cast<XiaomiOpcode>(command.opcode);
        message.sequence = sequence++;
        message.payload = command.payload;

        const auto bytes = EncodeXiaomiMessage(message);
        std::cout << "[" << (index + 1) << "/" << commands.size() << "] "
                  << command.label
                  << " opcode=" << ByteToHex(command.opcode)
                  << " payload=" << BytesToHex(command.payload) << "\n";

        if (!SendAll(socket_handle, bytes)) {
            std::cout << "Send failed for candidate " << (index + 1) << "\n";
            continue;
        }

        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(command.pause_ms);
        while (std::chrono::steady_clock::now() < until) {
            const auto chunk = ReceiveChunk(socket_handle);
            if (!chunk.has_value()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            rx_buffer.insert(rx_buffer.end(), chunk->begin(), chunk->end());
            const auto messages = DecodeXiaomiMessages(&rx_buffer);
            for (const auto& response : messages) {
                std::cout << "  rx type=" << ByteToHex(static_cast<std::uint8_t>(response.type))
                          << " opcode=" << ByteToHex(static_cast<std::uint8_t>(response.opcode))
                          << " payload=" << BytesToHex(response.payload) << "\n";
            }
        }
    }

    closesocket(socket_handle);
    std::cout << "Probe finished.\n";
    return true;
}

bool WinRtBatteryProvider::ObserveXiaomiControlSession(const std::string& device_hint, int duration_seconds) {
    EnsureApartmentInitialized();

    BatteryQueryOptions options;
    options.include_disconnected = false;
    const auto devices = GetDevicesBattery(options);

    std::vector<std::pair<std::string, std::uint64_t>> candidates;
    std::unordered_set<std::uint64_t> seen_addresses;
    const std::string normalized_hint = ToLowerAscii(device_hint);
    for (const auto& entry : devices) {
        if (!entry.is_connected) {
            continue;
        }
        if (!IsLikelyXiaomiEarbuds(entry.device_name, entry.device_name, entry.device_id)) {
            continue;
        }
        if (!normalized_hint.empty()) {
            const std::string probe = ToLowerAscii(entry.device_name + " " + entry.device_id);
            if (probe.find(normalized_hint) == std::string::npos) {
                continue;
            }
        }

        const auto address = ParseBluetoothAddressFromDeviceId(entry.device_id);
        if (!address.has_value() || !seen_addresses.insert(*address).second) {
            continue;
        }
        candidates.emplace_back(entry.device_name, *address);
    }

    if (candidates.empty()) {
        std::cout << "No connected Xiaomi/Redmi earbuds candidates were found.\n";
        return false;
    }

    const auto& target = candidates.front();
    std::cout << "Observing device: " << target.first << " address=" << target.second << "\n";
    std::cout << "Observation window: " << duration_seconds << " seconds.\n";
    std::cout << "Now switch ANC/transparency/off on the earbuds or in the phone app.\n";

    ScopedWsa wsa;
    if (!wsa.started()) {
        std::cout << "WSAStartup failed.\n";
        return false;
    }

    SOCKET socket_handle = INVALID_SOCKET;
    std::string connected_path;
    if (!ConnectXiaomiControlSocket(target.second, &socket_handle, &connected_path)) {
        std::cout << "Failed to open Xiaomi control socket.\n";
        return false;
    }

    std::cout << "Connected via " << connected_path << "\n";

    std::uint8_t sequence = 0;
    if (!RunXiaomiAuthHandshake(socket_handle, &sequence)) {
        std::cout << "Auth handshake failed.\n";
        closesocket(socket_handle);
        return false;
    }

    XiaomiMessage run_info_request;
    run_info_request.type = XiaomiMessageType::kPhoneRequest;
    run_info_request.opcode = XiaomiOpcode::kGetDeviceRunInfo;
    run_info_request.sequence = sequence++;
    run_info_request.payload = {0xFF, 0xFF, 0xFF, 0xFF};
    SendAll(socket_handle, EncodeXiaomiMessage(run_info_request));

    XiaomiMessage info_request;
    info_request.type = XiaomiMessageType::kPhoneRequest;
    info_request.opcode = XiaomiOpcode::kGetDeviceInfo;
    info_request.sequence = sequence++;
    info_request.payload = {0xFF, 0xFF, 0xFF, 0xFF};
    SendAll(socket_handle, EncodeXiaomiMessage(info_request));

    std::vector<std::uint8_t> rx_buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto chunk = ReceiveChunk(socket_handle);
        if (!chunk.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        std::cout << "chunk: " << BytesToHex(*chunk) << "\n";
        rx_buffer.insert(rx_buffer.end(), chunk->begin(), chunk->end());
        const auto messages = DecodeXiaomiMessages(&rx_buffer);
        for (const auto& response : messages) {
            std::cout << "rx type=" << ByteToHex(static_cast<std::uint8_t>(response.type))
                      << " opcode=" << ByteToHex(static_cast<std::uint8_t>(response.opcode))
                      << " seq=" << static_cast<int>(response.sequence)
                      << " payload=" << BytesToHex(response.payload) << "\n";

            if (response.type == XiaomiMessageType::kEarbudsNotify &&
                response.opcode == XiaomiOpcode::kReportStatus) {
                XiaomiMessage ack;
                ack.type = XiaomiMessageType::kResponse;
                ack.opcode = XiaomiOpcode::kReportStatus;
                ack.sequence = response.sequence;
                SendAll(socket_handle, EncodeXiaomiMessage(ack));
            }
        }
    }

    closesocket(socket_handle);
    std::cout << "Observation finished.\n";
    return true;
}

bool WinRtBatteryProvider::ObserveZmiSerialSession(const std::string& device_hint, int duration_seconds) {
    EnsureApartmentInitialized();

    BatteryQueryOptions options;
    options.include_disconnected = false;
    const auto devices = GetDevicesBattery(options);

    std::vector<std::pair<std::string, std::uint64_t>> candidates;
    std::unordered_set<std::uint64_t> seen_addresses;
    const std::string normalized_hint = ToLowerAscii(device_hint);
    for (const auto& entry : devices) {
        if (!entry.is_connected) {
            continue;
        }
        if (!normalized_hint.empty()) {
            const std::string probe = ToLowerAscii(entry.device_name + " " + entry.device_id);
            if (probe.find(normalized_hint) == std::string::npos) {
                continue;
            }
        } else if (ToLowerAscii(entry.device_name).find("zmi") == std::string::npos &&
                   ToLowerAscii(entry.device_name).find("purpods") == std::string::npos) {
            continue;
        }

        const auto address = ParseBluetoothAddressFromDeviceId(entry.device_id);
        if (!address.has_value() || !seen_addresses.insert(*address).second) {
            continue;
        }
        candidates.emplace_back(entry.device_name, *address);
    }

    if (candidates.empty()) {
        std::cout << "No connected ZMI candidates were found.\n";
        return false;
    }

    const auto& target = candidates.front();
    std::cout << "Observing ZMI serial device: " << target.first << " address=" << target.second << "\n";
    std::cout << "Observation window: " << duration_seconds << " seconds.\n";

    ScopedWsa wsa;
    if (!wsa.started()) {
        std::cout << "WSAStartup failed.\n";
        return false;
    }

    SOCKET socket_handle = INVALID_SOCKET;
    std::string connected_path;
    if (!ConnectXiaomiControlSocket(target.second, &socket_handle, &connected_path)) {
        std::cout << "Failed to open ZMI serial/control socket.\n";
        return false;
    }

    std::cout << "Connected via " << connected_path << "\n";

    auto make_probe_message =
        [](XiaomiMessageType type,
           XiaomiOpcode opcode,
           std::uint8_t sequence,
           std::initializer_list<std::uint8_t> payload) {
            XiaomiMessage message;
            message.type = type;
            message.opcode = opcode;
            message.sequence = sequence;
            message.payload.assign(payload.begin(), payload.end());
            return EncodeXiaomiMessage(message);
        };

    std::vector<std::vector<std::uint8_t>> probes;
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0x02, 0x00, 0x05, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x42});
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0x09, 0x00, 0x05, 0x01,
        0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x42});
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0x02, 0x00, 0x05, 0x02,
        0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x42});
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0x09, 0x00, 0x05, 0x08,
        0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x42});
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0xF1, 0x00, 0x07, 0x03,
        0x5A, 0x4D, 0xEA, 0x02, 0x81, 0x00, 0xEF, 0x5E});
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0xF1, 0x00, 0x07, 0x04,
        0x5A, 0x4D, 0xEA, 0x02, 0x81, 0x00, 0xEF, 0x5E});
    probes.push_back(std::vector<std::uint8_t>{
        0xFE, 0xDC, 0xBA, 0xC3, 0xF1, 0x00, 0x07, 0x05,
        0x5A, 0x4D, 0xEA, 0x02, 0x81, 0x00, 0xEF, 0x5E});

    constexpr std::array<const char*, 9> kTextProbes = {
        "AT\r",
        "AT+XAPL=ABCD-1234-0100,7\r",
        "AT+IPHONEACCEV?\r",
        "AT+XEVENT?\r",
        "AT+XEVENT=BATTERY?\r",
        "AT+XEVENT=GETBATTERY\r",
        "AT+BATT?\r",
        "AT+BATTERY?\r",
        "AT+STATUS?\r",
    };

    int probe_index = 1;
    for (const auto& probe : probes) {
        std::cout << "tx probe[" << probe_index++ << "]: " << BytesToHex(probe) << "\n";
        SendAll(socket_handle, probe);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    for (const char* probe : kTextProbes) {
        const std::string line = probe;
        send(socket_handle, line.data(), static_cast<int>(line.size()), 0);
        std::cout << "tx text: " << line;
        if (line.empty() || line.back() != '\n') {
            std::cout << "\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }

    std::vector<std::uint8_t> rx_buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto chunk = ReceiveChunk(socket_handle);
        if (!chunk.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            continue;
        }

        std::cout << "chunk: " << BytesToHex(*chunk) << "\n";

        if (const auto raw_extracted = ExtractPreferredXiaomiBatterySnapshot(*chunk); raw_extracted.has_value()) {
            const auto left_text = raw_extracted->left.has_value() ? std::to_string(*raw_extracted->left) : "na";
            const auto right_text = raw_extracted->right.has_value() ? std::to_string(*raw_extracted->right) : "na";
            const auto case_text =
                raw_extracted->case_level.has_value() ? std::to_string(*raw_extracted->case_level) : "na";
            std::cout << "raw battery candidate left=" << left_text
                      << " right=" << right_text
                      << " case=" << case_text << "\n";
        }

        rx_buffer.insert(rx_buffer.end(), chunk->begin(), chunk->end());
        const auto messages = DecodeXiaomiMessages(&rx_buffer);
        for (const auto& response : messages) {
            std::cout << "rx type=" << ByteToHex(static_cast<std::uint8_t>(response.type))
                      << " opcode=" << ByteToHex(static_cast<std::uint8_t>(response.opcode))
                      << " seq=" << static_cast<int>(response.sequence)
                      << " payload=" << BytesToHex(response.payload) << "\n";

            if (const auto extracted = ExtractPreferredXiaomiBatterySnapshot(response.payload); extracted.has_value()) {
                const auto left_text = extracted->left.has_value() ? std::to_string(*extracted->left) : "na";
                const auto right_text = extracted->right.has_value() ? std::to_string(*extracted->right) : "na";
                const auto case_text =
                    extracted->case_level.has_value() ? std::to_string(*extracted->case_level) : "na";
                std::cout << "payload battery candidate left=" << left_text
                          << " right=" << right_text
                          << " case=" << case_text << "\n";
            }
        }
    }

    closesocket(socket_handle);
    std::cout << "Observation finished.\n";
    return true;
}

bool WinRtBatteryProvider::DumpBluetoothServices(const std::string& device_hint) {
    EnsureApartmentInitialized();

    BatteryQueryOptions options;
    options.include_disconnected = true;
    const auto devices = GetDevicesBattery(options);

    std::vector<std::pair<std::string, std::uint64_t>> candidates;
    std::unordered_set<std::uint64_t> seen_addresses;
    const std::string normalized_hint = ToLowerAscii(device_hint);
    for (const auto& entry : devices) {
        if (!normalized_hint.empty()) {
            const std::string probe = ToLowerAscii(entry.device_name + " " + entry.device_id);
            if (probe.find(normalized_hint) == std::string::npos) {
                continue;
            }
        }

        const auto address = ParseBluetoothAddressFromDeviceId(entry.device_id);
        if (!address.has_value() || !seen_addresses.insert(*address).second) {
            continue;
        }
        candidates.emplace_back(entry.device_name, *address);
    }

    if (candidates.empty()) {
        std::cout << "No Bluetooth candidates matched.\n";
        return false;
    }

    const auto& target = candidates.front();
    std::cout << "Bluetooth service dump for: " << target.first << " address=" << target.second << "\n";

    const auto dump_channels = [&](const GUID* service_filter, const char* label, bool flush_cache) {
        const auto channels = DiscoverRfcommChannelsFromSdp(target.second, service_filter, flush_cache);
        std::cout << label << ": " << channels.size() << " channel(s)\n";
        for (const auto& channel : channels) {
            std::cout << "  port=" << channel.port
                      << " uuid=" << GuidToString(winrt::guid(channel.service_uuid));
            if (!channel.instance_name.empty()) {
                std::cout << " name='" << channel.instance_name << "'";
            }
            std::cout << "\n";
        }
    };

    dump_channels(&kXiaomiDeviceCtrlServiceUuid, "FD2D", true);
    dump_channels(&kBluetoothSerialPortServiceUuid, "SPP-1101", false);
    dump_channels(&kZmiPurPodsSerialServiceUuid, "ZMI-1101", false);
    dump_channels(&kHandsfreeAudioGatewayServiceUuid, "HFP-111E", false);
    dump_channels(nullptr, "SDP-ANY", false);
    return true;
}

bool WinRtBatteryProvider::DumpBleGatt(const std::string& device_hint) {
    EnsureApartmentInitialized();

    const auto candidates = EnumerateCandidateDevices();
    const std::string normalized_hint = ToLowerAscii(device_hint);
    bool dumped_any = false;

    if (candidates.empty()) {
        std::cout << "No BLE candidates matched.\n";
        return false;
    }

    for (const auto& device_info : candidates) {
        const std::string candidate_id = ToUtf8(device_info.Id());
        const std::string candidate_name = ToUtf8(device_info.Name());
        const std::string probe = ToLowerAscii(candidate_name + " " + candidate_id);
        if (!normalized_hint.empty() && probe.find(normalized_hint) == std::string::npos) {
            continue;
        }

        dumped_any = true;
        std::cout << "BLE GATT dump for id='" << candidate_id << "' name='" << candidate_name << "'\n";

        const auto maybe_device =
            WaitForAsyncResult(BluetoothLEDevice::FromIdAsync(device_info.Id()), std::chrono::milliseconds(1800));
        if (!maybe_device.has_value() || !(*maybe_device)) {
            std::cout << "  open by id failed\n";
            continue;
        }
        const auto device = *maybe_device;

        auto services_result =
            WaitForAsyncResult(device.GetGattServicesAsync(BluetoothCacheMode::Uncached),
                               std::chrono::milliseconds(2200));
        if (!services_result.has_value() || services_result->Status() != GattCommunicationStatus::Success) {
            services_result =
                WaitForAsyncResult(device.GetGattServicesAsync(BluetoothCacheMode::Cached),
                                   std::chrono::milliseconds(2200));
        }
        if (!services_result.has_value()) {
            std::cout << "  GATT service enumeration timed out.\n";
            continue;
        }
        if (services_result->Status() != GattCommunicationStatus::Success) {
            std::cout << "  GATT service enumeration failed with status="
                      << static_cast<int>(services_result->Status()) << "\n";
            continue;
        }

        std::cout << "  Services: " << services_result->Services().Size() << "\n";
        for (const auto& service : services_result->Services()) {
            const std::string service_uuid = ToLowerAscii(ToUtf8(winrt::to_hstring(service.Uuid())));
            std::cout << "  service " << service_uuid << "\n";

            auto characteristics_result =
                WaitForAsyncResult(service.GetCharacteristicsAsync(BluetoothCacheMode::Uncached),
                                   std::chrono::milliseconds(1500));
            if (!characteristics_result.has_value() ||
                characteristics_result->Status() != GattCommunicationStatus::Success) {
                characteristics_result =
                    WaitForAsyncResult(service.GetCharacteristicsAsync(BluetoothCacheMode::Cached),
                                       std::chrono::milliseconds(1500));
            }
            if (!characteristics_result.has_value()) {
                std::cout << "    characteristics: timeout\n";
                continue;
            }
            if (characteristics_result->Status() != GattCommunicationStatus::Success) {
                std::cout << "    characteristics: status=" << static_cast<int>(characteristics_result->Status())
                          << "\n";
                continue;
            }

            for (const auto& characteristic : characteristics_result->Characteristics()) {
                const std::string char_uuid = ToLowerAscii(ToUtf8(winrt::to_hstring(characteristic.Uuid())));
                const auto props = static_cast<unsigned int>(characteristic.CharacteristicProperties());
                std::cout << "    char " << char_uuid << " props=0x" << std::hex << props << std::dec;

                const auto user_description = ToUtf8(characteristic.UserDescription());
                if (!user_description.empty()) {
                    std::cout << " desc='" << user_description << "'";
                }
                std::cout << "\n";

                if ((characteristic.CharacteristicProperties() & GattCharacteristicProperties::Read) !=
                    GattCharacteristicProperties::Read) {
                    continue;
                }

                auto read_result =
                    WaitForAsyncResult(characteristic.ReadValueAsync(BluetoothCacheMode::Uncached),
                                       std::chrono::milliseconds(900));
                if (!read_result.has_value() || read_result->Status() != GattCommunicationStatus::Success) {
                    read_result = WaitForAsyncResult(characteristic.ReadValueAsync(BluetoothCacheMode::Cached),
                                                     std::chrono::milliseconds(900));
                }
                if (!read_result.has_value() || read_result->Status() != GattCommunicationStatus::Success) {
                    std::cout << "      read failed\n";
                    continue;
                }

                const auto bytes = ReadBufferBytes(read_result->Value());
                std::cout << "      len=" << bytes.size() << " data=" << BytesToHex(bytes) << "\n";
            }
        }
    }

    if (!dumped_any) {
        std::cout << "No BLE candidates matched.\n";
        return false;
    }

    return true;
}

bool WinRtBatteryProvider::SetXiaomiNoiseMode(const std::string& mode, const std::string& device_hint) {
    EnsureApartmentInitialized();

    const std::string normalized_mode = ToLowerAscii(mode);
    std::uint8_t mode_value = 0;
    std::uint8_t f4_tail_value = 0;
    if (normalized_mode == "off" || normalized_mode == "disable" || normalized_mode == "disabled") {
        mode_value = 0x00;
        f4_tail_value = 0x00;
    } else if (normalized_mode == "anc" || normalized_mode == "noise" || normalized_mode == "noise-canceling") {
        mode_value = 0x01;
        f4_tail_value = 0x02;
    } else if (normalized_mode == "transparency" || normalized_mode == "transparent") {
        mode_value = 0x02;
        f4_tail_value = 0x01;
    } else {
        std::cout << "Unknown mode. Use one of: off, anc, transparency\n";
        return false;
    }

    BatteryQueryOptions options;
    options.include_disconnected = false;
    const auto devices = GetDevicesBattery(options);

    std::vector<std::pair<std::string, std::uint64_t>> candidates;
    std::unordered_set<std::uint64_t> seen_addresses;
    const std::string normalized_hint = ToLowerAscii(device_hint);
    for (const auto& entry : devices) {
        if (!entry.is_connected) {
            continue;
        }
        if (!IsLikelyXiaomiEarbuds(entry.device_name, entry.device_name, entry.device_id)) {
            continue;
        }
        if (!normalized_hint.empty()) {
            const std::string probe = ToLowerAscii(entry.device_name + " " + entry.device_id);
            if (probe.find(normalized_hint) == std::string::npos) {
                continue;
            }
        }

        const auto address = ParseBluetoothAddressFromDeviceId(entry.device_id);
        if (!address.has_value() || !seen_addresses.insert(*address).second) {
            continue;
        }
        candidates.emplace_back(entry.device_name, *address);
    }

    if (candidates.empty()) {
        std::cout << "No connected Xiaomi/Redmi earbuds candidates were found.\n";
        return false;
    }

    const auto& target = candidates.front();
    std::cout << "Setting mode on device: " << target.first << " address=" << target.second << "\n";
    std::cout << "Requested mode: " << normalized_mode << " (experimental)\n";

    ScopedWsa wsa;
    if (!wsa.started()) {
        std::cout << "WSAStartup failed.\n";
        return false;
    }

    SOCKET socket_handle = INVALID_SOCKET;
    std::string connected_path;
    if (!ConnectXiaomiControlSocket(target.second, &socket_handle, &connected_path)) {
        std::cout << "Failed to open Xiaomi control socket.\n";
        return false;
    }

    std::cout << "Connected via " << connected_path << "\n";

    std::uint8_t sequence = 0;
    if (!RunXiaomiAuthHandshake(socket_handle, &sequence)) {
        std::cout << "Auth handshake failed.\n";
        closesocket(socket_handle);
        return false;
    }

    const auto send_command = [&](std::uint8_t raw_type,
                                  std::uint8_t raw_opcode,
                                  std::initializer_list<std::uint8_t> payload) -> bool {
        XiaomiMessage message;
        message.type = static_cast<XiaomiMessageType>(raw_type);
        message.opcode = static_cast<XiaomiOpcode>(raw_opcode);
        message.sequence = sequence++;
        message.payload.assign(payload.begin(), payload.end());

        std::cout << "Sending type=" << ByteToHex(raw_type)
                  << " opcode=" << ByteToHex(raw_opcode)
                  << " payload=" << BytesToHex(message.payload) << "\n";
        return SendAll(socket_handle, EncodeXiaomiMessage(message));
    };

    if (!send_command(0x01U, 0x0EU, {0x02, 0x04, mode_value})) {
        std::cout << "Send failed for opcode 0x0E.\n";
        closesocket(socket_handle);
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    if (!send_command(0x01U, 0xF4U, {0x04, 0x00, 0x0B, mode_value, f4_tail_value})) {
        std::cout << "Send failed for opcode 0xF4.\n";
        closesocket(socket_handle);
        return false;
    }

    std::vector<std::uint8_t> rx_buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto chunk = ReceiveChunk(socket_handle);
        if (!chunk.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        std::cout << "chunk: " << BytesToHex(*chunk) << "\n";
        rx_buffer.insert(rx_buffer.end(), chunk->begin(), chunk->end());
        const auto messages = DecodeXiaomiMessages(&rx_buffer);
        for (const auto& response : messages) {
            std::cout << "rx type=" << ByteToHex(static_cast<std::uint8_t>(response.type))
                      << " opcode=" << ByteToHex(static_cast<std::uint8_t>(response.opcode))
                      << " seq=" << static_cast<int>(response.sequence)
                      << " payload=" << BytesToHex(response.payload) << "\n";

            if (response.type == XiaomiMessageType::kEarbudsNotify &&
                response.opcode == XiaomiOpcode::kReportStatus) {
                XiaomiMessage ack;
                ack.type = XiaomiMessageType::kResponse;
                ack.opcode = XiaomiOpcode::kReportStatus;
                ack.sequence = response.sequence;
                SendAll(socket_handle, EncodeXiaomiMessage(ack));
            }
        }
    }

    closesocket(socket_handle);
    std::cout << "Command finished.\n";
    return true;
}

bool WinRtBatteryProvider::SupportsNoiseControl(const std::string& device_id) {
    return ParseBluetoothAddressFromDeviceId(device_id).has_value();
}

bool WinRtBatteryProvider::SetNoiseControlMode(const std::string& device_id, NoiseControlMode mode) {
    const auto address = ParseBluetoothAddressFromDeviceId(device_id);
    if (!address.has_value()) {
        return false;
    }

    EnsureApartmentInitialized();
    ScopedWsa wsa;
    if (!wsa.started()) {
        return false;
    }

    SOCKET socket_handle = INVALID_SOCKET;
    std::string connected_path;
    if (!ConnectXiaomiControlSocket(*address, &socket_handle, &connected_path)) {
        return false;
    }

    std::uint8_t sequence = 0;
    if (!RunXiaomiAuthHandshake(socket_handle, &sequence)) {
        closesocket(socket_handle);
        return false;
    }

    std::uint8_t mode_value = 0;
    switch (mode) {
        case NoiseControlMode::Off:
            mode_value = 0x00;
            break;
        case NoiseControlMode::Anc:
            mode_value = 0x01;
            break;
        case NoiseControlMode::Transparency:
            mode_value = 0x02;
            break;
    }

    XiaomiMessage message;
    message.type = static_cast<XiaomiMessageType>(0xC1U);
    message.opcode = static_cast<XiaomiOpcode>(0x08U);
    message.sequence = sequence++;
    message.payload = {0x02, 0x04, mode_value};
    const bool sent = SendAll(socket_handle, EncodeXiaomiMessage(message));
    if (!sent) {
        closesocket(socket_handle);
        return false;
    }

    bool observed_confirmation = false;
    std::vector<std::uint8_t> rx_buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1800);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto chunk = ReceiveChunk(socket_handle);
        if (!chunk.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            continue;
        }
        rx_buffer.insert(rx_buffer.end(), chunk->begin(), chunk->end());
        const auto messages = DecodeXiaomiMessages(&rx_buffer);
        for (const auto& response : messages) {
            const auto parsed_mode =
                ParseXiaomiNoiseModeCode(static_cast<std::uint8_t>(response.opcode), response.payload);
            if (parsed_mode.has_value()) {
                const auto parsed_submode =
                    static_cast<std::uint8_t>(response.opcode) == 0xF4U
                        ? ParseXiaomiNoiseSubmodeCodeFromF4Payload(response.payload)
                        : std::optional<std::uint8_t>{};
                PutXiaomiModeCacheEntry(*address, *parsed_mode, parsed_submode);
                if (*parsed_mode == mode_value) {
                    observed_confirmation = true;
                }
            }
        }
    }

    closesocket(socket_handle);
    return observed_confirmation;
}

bool WinRtBatteryProvider::SupportsNoiseSubmodes(const std::string& device_id, NoiseControlMode mode) {
    return SupportsNoiseControl(device_id) &&
           (mode == NoiseControlMode::Transparency || mode == NoiseControlMode::Anc);
}

std::vector<std::pair<std::string, std::string>> WinRtBatteryProvider::GetNoiseSubmodes(const std::string& device_id,
                                                                                         NoiseControlMode mode) {
    if (!SupportsNoiseSubmodes(device_id, mode)) {
        return {};
    }

    return {
        {"standard", "Прозрачность"},
        {"voice", "Усиление голоса"},
    };
}

bool WinRtBatteryProvider::SetNoiseSubmode(const std::string& device_id,
                                           NoiseControlMode mode,
                                           const std::string& submode_id) {
    if (!SupportsNoiseSubmodes(device_id, mode)) {
        return false;
    }

    const std::string normalized_submode = ToLowerAscii(submode_id);
    if (mode == NoiseControlMode::Transparency) {
        if (normalized_submode == "standard") {
            return SetXiaomiNoiseSubmode("transparency", 0, device_id);
        }
        if (normalized_submode == "voice") {
            return SetXiaomiNoiseSubmode("transparency", 1, device_id);
        }
        return false;
    }

    if (mode == NoiseControlMode::Anc) {
        if (normalized_submode == "balanced") {
            return SetXiaomiNoiseSubmode("anc", 0, device_id);
        }
        if (normalized_submode == "weak") {
            return SetXiaomiNoiseSubmode("anc", 1, device_id);
        }
        if (normalized_submode == "deep") {
            return SetXiaomiNoiseSubmode("anc", 2, device_id);
        }
        if (normalized_submode == "adaptive") {
            return SetXiaomiNoiseSubmode("anc", 3, device_id);
        }
    }

    return false;
}

bool WinRtBatteryProvider::SendXiaomiControlCandidate(int candidate_id, const std::string& device_hint) {
    struct Candidate {
        int id = 0;
        std::uint8_t opcode = 0;
        std::vector<std::uint8_t> payload;
        const char* label = "";
    };

    const std::vector<Candidate> candidates = {
        {1, 0x08, {0x02, 0x04, 0x00}, "c1-08 off"},
        {2, 0x08, {0x02, 0x04, 0x01}, "c1-08 anc"},
        {3, 0x08, {0x02, 0x04, 0x02}, "c1-08 transparency"},
        {4, 0xF4, {0x04, 0x00, 0x0B, 0x00, 0x00}, "f4 style off"},
        {5, 0xF4, {0x04, 0x00, 0x0B, 0x01, 0x00}, "f4 style anc"},
        {6, 0xF4, {0x04, 0x00, 0x0B, 0x02, 0x01}, "f4 style transparency"},
        {7, 0xF4, {0x04, 0x00, 0x0B, 0x01, 0x01}, "f4 alt anc"},
        {8, 0xF4, {0x04, 0x00, 0x0B, 0x02, 0x00}, "f4 alt transparency"},
        {9, 0xF4, {0x04, 0x00, 0x0B, 0x00, 0x01}, "f4 alt off"},
        {10, 0xF4, {0x04, 0x00, 0x0B, 0x00, 0x00}, "f4 off as response"},
        {11, 0xF4, {0x04, 0x00, 0x0B, 0x01, 0x00}, "f4 anc as response"},
        {12, 0xF4, {0x04, 0x00, 0x0B, 0x02, 0x01}, "f4 transparency as response"},
        {13, 0xF4, {0x04, 0x00, 0x0B, 0x00, 0x00}, "f4 off as earbuds-request"},
        {14, 0xF4, {0x04, 0x00, 0x0B, 0x01, 0x00}, "f4 anc as earbuds-request"},
        {15, 0xF4, {0x04, 0x00, 0x0B, 0x02, 0x01}, "f4 transparency as earbuds-request"},
    };

    const auto candidate_it = std::find_if(candidates.begin(), candidates.end(),
                                           [candidate_id](const Candidate& candidate) {
                                               return candidate.id == candidate_id;
                                           });
    if (candidate_it == candidates.end()) {
        std::cout << "Unknown candidate. Use 1..9\n";
        return false;
    }

    EnsureApartmentInitialized();
    BatteryQueryOptions options;
    options.include_disconnected = false;
    const auto devices = GetDevicesBattery(options);

    std::vector<std::pair<std::string, std::uint64_t>> matched_devices;
    std::unordered_set<std::uint64_t> seen_addresses;
    const std::string normalized_hint = ToLowerAscii(device_hint);
    for (const auto& entry : devices) {
        if (!entry.is_connected) {
            continue;
        }
        if (!IsLikelyXiaomiEarbuds(entry.device_name, entry.device_name, entry.device_id)) {
            continue;
        }
        if (!normalized_hint.empty()) {
            const std::string probe = ToLowerAscii(entry.device_name + " " + entry.device_id);
            if (probe.find(normalized_hint) == std::string::npos) {
                continue;
            }
        }

        const auto address = ParseBluetoothAddressFromDeviceId(entry.device_id);
        if (!address.has_value() || !seen_addresses.insert(*address).second) {
            continue;
        }
        matched_devices.emplace_back(entry.device_name, *address);
    }

    if (matched_devices.empty()) {
        std::cout << "No connected Xiaomi/Redmi earbuds candidates were found.\n";
        return false;
    }

    const auto& target = matched_devices.front();
    std::cout << "Testing candidate #" << candidate_it->id << " on device: " << target.first << "\n";
    std::cout << "Label: " << candidate_it->label << "\n";
    std::cout << "Opcode=" << ByteToHex(candidate_it->opcode)
              << " payload=" << BytesToHex(candidate_it->payload) << "\n";

    ScopedWsa wsa;
    if (!wsa.started()) {
        std::cout << "WSAStartup failed.\n";
        return false;
    }

    SOCKET socket_handle = INVALID_SOCKET;
    std::string connected_path;
    if (!ConnectXiaomiControlSocket(target.second, &socket_handle, &connected_path)) {
        std::cout << "Failed to open Xiaomi control socket.\n";
        return false;
    }

    std::cout << "Connected via " << connected_path << "\n";

    std::uint8_t sequence = 0;
    if (!RunXiaomiAuthHandshake(socket_handle, &sequence)) {
        std::cout << "Auth handshake failed.\n";
        closesocket(socket_handle);
        return false;
    }

    XiaomiMessage message;
    if (candidate_it->id <= 3) {
        message.type = static_cast<XiaomiMessageType>(0xC1U);
    } else if (candidate_it->id >= 13) {
        message.type = XiaomiMessageType::kEarbudsRequest;
    } else if (candidate_it->id >= 10) {
        message.type = XiaomiMessageType::kResponse;
    } else {
        message.type = XiaomiMessageType::kPhoneRequest;
    }
    message.opcode = static_cast<XiaomiOpcode>(candidate_it->opcode);
    message.sequence = sequence++;
    message.payload = candidate_it->payload;

    if (!SendAll(socket_handle, EncodeXiaomiMessage(message))) {
        std::cout << "Send failed.\n";
        closesocket(socket_handle);
        return false;
    }

    std::vector<std::uint8_t> rx_buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto chunk = ReceiveChunk(socket_handle);
        if (!chunk.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        std::cout << "chunk: " << BytesToHex(*chunk) << "\n";
        rx_buffer.insert(rx_buffer.end(), chunk->begin(), chunk->end());
        const auto messages = DecodeXiaomiMessages(&rx_buffer);
        for (const auto& response : messages) {
            std::cout << "rx type=" << ByteToHex(static_cast<std::uint8_t>(response.type))
                      << " opcode=" << ByteToHex(static_cast<std::uint8_t>(response.opcode))
                      << " seq=" << static_cast<int>(response.sequence)
                      << " payload=" << BytesToHex(response.payload) << "\n";

            if (response.type == XiaomiMessageType::kEarbudsNotify &&
                response.opcode == XiaomiOpcode::kReportStatus) {
                XiaomiMessage ack;
                ack.type = XiaomiMessageType::kResponse;
                ack.opcode = XiaomiOpcode::kReportStatus;
                ack.sequence = response.sequence;
                SendAll(socket_handle, EncodeXiaomiMessage(ack));
            }
        }
    }

    closesocket(socket_handle);
    std::cout << "Candidate finished.\n";
    return true;
}

bool WinRtBatteryProvider::SetXiaomiNoiseSubmode(const std::string& family, int submode, const std::string& device_hint) {
    const std::string normalized_family = ToLowerAscii(family);
    std::uint8_t family_code = 0;
    int max_submode = 0;
    if (normalized_family == "transparency" || normalized_family == "transparent") {
        family_code = 0x02;
        max_submode = 1;
    } else if (normalized_family == "anc") {
        family_code = 0x01;
        max_submode = 3;
    } else {
        std::cout << "Unknown family. Use anc or transparency\n";
        return false;
    }

    if (submode < 0 || submode > max_submode) {
        std::cout << "Submode out of range.\n";
        return false;
    }

    BatteryQueryOptions options;
    options.include_disconnected = false;
    const auto devices = GetDevicesBattery(options);

    std::vector<std::pair<std::string, std::uint64_t>> candidates;
    std::unordered_set<std::uint64_t> seen_addresses;
    const std::string normalized_hint = ToLowerAscii(device_hint);
    for (const auto& entry : devices) {
        if (!entry.is_connected) {
            continue;
        }
        if (!IsLikelyXiaomiEarbuds(entry.device_name, entry.device_name, entry.device_id)) {
            continue;
        }
        if (!normalized_hint.empty()) {
            const std::string probe = ToLowerAscii(entry.device_name + " " + entry.device_id);
            if (probe.find(normalized_hint) == std::string::npos) {
                continue;
            }
        }
        const auto address = ParseBluetoothAddressFromDeviceId(entry.device_id);
        if (!address.has_value() || !seen_addresses.insert(*address).second) {
            continue;
        }
        candidates.emplace_back(entry.device_name, *address);
    }

    if (candidates.empty()) {
        std::cout << "No connected Xiaomi/Redmi earbuds candidates were found.\n";
        return false;
    }

    ScopedWsa wsa;
    if (!wsa.started()) {
        return false;
    }
    SOCKET socket_handle = INVALID_SOCKET;
    std::string connected_path;
    if (!ConnectXiaomiControlSocket(candidates.front().second, &socket_handle, &connected_path)) {
        return false;
    }

    std::uint8_t sequence = 0;
    if (!RunXiaomiAuthHandshake(socket_handle, &sequence)) {
        closesocket(socket_handle);
        return false;
    }

    XiaomiMessage message;
    message.type = static_cast<XiaomiMessageType>(0xC1U);
    message.opcode = static_cast<XiaomiOpcode>(0xF2U);
    message.sequence = sequence++;
    message.payload = {0x04, 0x00, 0x0B, family_code, static_cast<std::uint8_t>(submode)};
    std::cout << "Sending submode family=" << normalized_family
              << " submode=" << submode
              << " payload=" << BytesToHex(message.payload) << "\n";
    const bool sent = SendAll(socket_handle, EncodeXiaomiMessage(message));

    std::vector<std::uint8_t> rx_buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto chunk = ReceiveChunk(socket_handle);
        if (!chunk.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            continue;
        }
        std::cout << "chunk: " << BytesToHex(*chunk) << "\n";
        rx_buffer.insert(rx_buffer.end(), chunk->begin(), chunk->end());
        const auto messages = DecodeXiaomiMessages(&rx_buffer);
        for (const auto& response : messages) {
            std::cout << "rx type=" << ByteToHex(static_cast<std::uint8_t>(response.type))
                      << " opcode=" << ByteToHex(static_cast<std::uint8_t>(response.opcode))
                      << " seq=" << static_cast<int>(response.sequence)
                      << " payload=" << BytesToHex(response.payload) << "\n";
        }
    }

    closesocket(socket_handle);
    if (sent) {
        PutXiaomiModeCacheEntry(candidates.front().second,
                                family_code == 0x01U ? 0x01U : 0x02U,
                                static_cast<std::uint8_t>(submode));
    }
    return sent;
}

}  // namespace battery_monitor
