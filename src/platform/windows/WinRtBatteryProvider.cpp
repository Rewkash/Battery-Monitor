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
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <iostream>

#include <winsock2.h>
#include <ws2bth.h>
#include <windows.h>
#include <cfgmgr32.h>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#pragma comment(lib, "Cfgmgr32.lib")

namespace battery_monitor {

namespace {

using winrt::Windows::Devices::Bluetooth::BluetoothCacheMode;
using winrt::Windows::Devices::Bluetooth::BluetoothAddressType;
using winrt::Windows::Devices::Bluetooth::BluetoothDevice;
using winrt::Windows::Devices::Bluetooth::BluetoothConnectionStatus;
using winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristicProperties;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristicUuids;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattServiceUuids;
using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Devices::Enumeration::DeviceInformationKind;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncOperation;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::Storage::Streams::DataReader;

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
    bool from_connected_scan = false;
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
constexpr DEVPROPKEY kZmiVendorBatteryHintPropKey = {
    {0x670245F9, 0x6E25, 0x4179, {0x85, 0xC1, 0x98, 0x1C, 0x33, 0xB9, 0xD3, 0xB7}},
    4};

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

bool PersistentXiaomiCacheEnabled() {
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

std::vector<std::wstring> FindBthEnumInstanceIdsByAddress(std::uint64_t address) {
    std::wstringstream filter_builder;
    filter_builder << L"BTHENUM\\DEV_" << std::uppercase << std::hex
                   << std::setw(12) << std::setfill(L'0') << address << L"\\*";
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
        instance_ids.emplace_back(current, length);
        cursor += length + 1U;
    }

    return instance_ids;
}

std::optional<std::uint8_t> ReadZmiVendorBatteryHintFromPnpAddress(std::uint64_t address) {
    const auto instance_ids = FindBthEnumInstanceIdsByAddress(address);
    if (instance_ids.empty()) {
        DebugLog("ZMI PnP lookup: instance id not found for address=" + std::to_string(address));
        return std::nullopt;
    }

    std::array<std::uint8_t, 16> raw_data{};

    auto normalize = [](int raw_value) -> std::optional<std::uint8_t> {
        if (raw_value <= 0 || raw_value == 255) {
            return std::nullopt;
        }
        if (raw_value <= 10) {
            return static_cast<std::uint8_t>(raw_value * 10);
        }
        if (raw_value <= 100) {
            return static_cast<std::uint8_t>(raw_value);
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

        DEVPROPTYPE property_type = DEVPROP_TYPE_EMPTY;
        ULONG size = static_cast<ULONG>(raw_data.size());
        const auto property_result = CM_Get_DevNode_PropertyW(
            dev_inst, &kZmiVendorBatteryHintPropKey, &property_type,
            reinterpret_cast<PBYTE>(raw_data.data()), &size, 0);
        if (property_result != CR_SUCCESS) {
            continue;
        }

        if (property_type == DEVPROP_TYPE_BYTE && size >= 1U) {
            const auto normalized = normalize(static_cast<int>(raw_data[0]));
            if (normalized.has_value()) {
                DebugLog("ZMI PnP lookup: vendor property byte=" + std::to_string(raw_data[0]));
                return normalized;
            }
            continue;
        }
        if (property_type == DEVPROP_TYPE_UINT32 && size >= sizeof(std::uint32_t)) {
            std::uint32_t value = 0;
            std::memcpy(&value, raw_data.data(), sizeof(value));
            const auto normalized = normalize(static_cast<int>(value));
            if (normalized.has_value()) {
                DebugLog("ZMI PnP lookup: vendor property uint32=" + std::to_string(value));
                return normalized;
            }
            continue;
        }
        if (property_type == DEVPROP_TYPE_INT32 && size >= sizeof(std::int32_t)) {
            std::int32_t value = 0;
            std::memcpy(&value, raw_data.data(), sizeof(value));
            const auto normalized = normalize(static_cast<int>(value));
            if (normalized.has_value()) {
                DebugLog("ZMI PnP lookup: vendor property int32=" + std::to_string(value));
                return normalized;
            }
            continue;
        }
    }

    DebugLog("ZMI PnP lookup: vendor property missing for address=" + std::to_string(address));
    return std::nullopt;
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
        if (len != 4U || tag > 0x1FU) {
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
        if (!preferred_tag.has_value() && (tag == 0x00U || tag == 0x07U)) {
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

std::optional<std::uint8_t> TryReadHfpBatteryFromSocket(SOCKET socket_handle) {
    if (socket_handle == INVALID_SOCKET) {
        return std::nullopt;
    }

    constexpr std::array<const char*, 6> kProbeCommands = {
        "AT+BRSF=20\r",
        "AT+CIND=?\r",
        "AT+CIND?\r",
        "AT+IPHONEACCEV?\r",
        "AT+XEVENT?\r",
        "AT+BIEV?\r",
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

        std::this_thread::sleep_for(std::chrono::milliseconds(90));
        drain_input(3);

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
    if (!PersistentXiaomiCacheEnabled() || !HasAnyBattery(snapshot)) {
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
    if (!PersistentXiaomiCacheEnabled()) {
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

std::vector<BatteryReading> TryReadXiaomiClassicBattery(std::uint64_t bluetooth_address) {
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
        const SOCKET candidate_socket = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
        if (candidate_socket == INVALID_SOCKET) {
            DebugLog("Xiaomi classic fallback: socket() failed for " + std::string(service_name) +
                     " error=" + std::to_string(WSAGetLastError()));
            return false;
        }

        const int timeout_ms = 280;
        setsockopt(candidate_socket, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
        setsockopt(candidate_socket, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

        auto service_address = address;
        service_address.serviceClassId = service_uuid;

        if (!ConnectWithTimeout(candidate_socket, service_address, timeout_ms)) {
            DebugLog("Xiaomi classic fallback: connect(" + std::string(service_name) +
                     ") failed error=" + std::to_string(WSAGetLastError()));
            closesocket(candidate_socket);
            return false;
        }

        socket_handle = candidate_socket;
        connected_path = service_name;
        DebugLog("Xiaomi classic fallback: RFCOMM connected via " + std::string(service_name));
        return true;
    };

    auto try_connect_port = [&](std::uint32_t port, const char* name) -> bool {
        const SOCKET candidate_socket = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
        if (candidate_socket == INVALID_SOCKET) {
            DebugLog("Xiaomi classic fallback: socket() failed for " + std::string(name) +
                     " error=" + std::to_string(WSAGetLastError()));
            return false;
        }

        const int timeout_ms = 280;
        setsockopt(candidate_socket, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
        setsockopt(candidate_socket, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

        auto service_address = address;
        service_address.serviceClassId = GUID{};
        service_address.port = port;

        if (!ConnectWithTimeout(candidate_socket, service_address, timeout_ms)) {
            DebugLog("Xiaomi classic fallback: connect(" + std::string(name) +
                     ") failed error=" + std::to_string(WSAGetLastError()));
            closesocket(candidate_socket);
            return false;
        }

        socket_handle = candidate_socket;
        connected_path = name;
        DebugLog("Xiaomi classic fallback: RFCOMM connected via " + std::string(name));
        return true;
    };

    if (!try_connect(kXiaomiDeviceCtrlServiceUuid, "FD2D") &&
        !try_connect(kBluetoothSerialPortServiceUuid, "SPP-1101") &&
        !try_connect(kZmiPurPodsSerialServiceUuid, "ZMI-1101") &&
        !try_connect(kHandsfreeAudioGatewayServiceUuid, "HFP-111E") &&
        !try_connect_port(15, "RFCOMM-port-15")) {
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

        const auto raw_extracted = ExtractBatterySnapshotFromXiaomiPayload(*chunk, std::nullopt);
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
                const auto extracted = ExtractBatterySnapshotFromXiaomiPayload(message.payload, std::nullopt);
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
            merged = MergeXiaomiSnapshots(merged, *generic_snapshot);
        }

        readings = BuildXiaomiBatteryReadings(merged);
        const std::string left_text = merged.left.has_value() ? std::to_string(*merged.left) : "na";
        const std::string right_text = merged.right.has_value() ? std::to_string(*merged.right) : "na";
        const std::string case_text = merged.case_level.has_value() ? std::to_string(*merged.case_level) : "na";
        DebugLog("Xiaomi classic fallback: battery merged left=" + left_text +
                 " right=" + right_text + " case=" + case_text);
    } else {
        DebugLog("Xiaomi classic fallback: timeout without battery payload");

        const bool likely_hfp_path = connected_path.find("HFP") != std::string::npos;
        std::optional<std::uint8_t> hfp_battery;
        if (likely_hfp_path) {
            hfp_battery = TryReadHfpBatteryFromSocket(socket_handle);
        } else {
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
                    hfp_battery = TryReadHfpBatteryFromSocket(hfp_socket);
                }
                closesocket(hfp_socket);
            }
        }

        if (hfp_battery.has_value()) {
            readings.push_back(BatteryReading{"main", *hfp_battery});
            DebugLog("Xiaomi classic fallback: HFP battery main=" + std::to_string(*hfp_battery));
        }
    }

    closesocket(socket_handle);
    return readings;
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

std::optional<std::uint8_t> NormalizeZmiVendorBatteryHint(int raw_value) {
    if (raw_value <= 0 || raw_value == 255) {
        return std::nullopt;
    }
    if (raw_value <= 10) {
        return static_cast<std::uint8_t>(raw_value * 10);
    }
    if (raw_value <= 100) {
        return static_cast<std::uint8_t>(raw_value);
    }
    return std::nullopt;
}

std::optional<std::uint8_t> ReadZmiVendorBatteryHint(const DeviceInformation& endpoint_info) {
    constexpr auto kVendorBatteryHintKey = L"{670245F9-6E25-4179-85C1-981C33B9D3B7} 4";

    IInspectable raw_value = nullptr;
    if (!TryGetPropertyValue(endpoint_info, kVendorBatteryHintKey, &raw_value)) {
        DebugLog("ZMI vendor key was not present in DeviceInformation properties");
        return std::nullopt;
    }

    try {
        const auto normalized = NormalizeZmiVendorBatteryHint(static_cast<int>(winrt::unbox_value<std::uint8_t>(raw_value)));
        if (!normalized.has_value()) {
            DebugLog("ZMI vendor key was present but value was not usable (uint8)");
        }
        return normalized;
    } catch (const winrt::hresult_error&) {
    }

    try {
        const auto normalized = NormalizeZmiVendorBatteryHint(static_cast<int>(winrt::unbox_value<std::uint32_t>(raw_value)));
        if (!normalized.has_value()) {
            DebugLog("ZMI vendor key was present but value was not usable (uint32)");
        }
        return normalized;
    } catch (const winrt::hresult_error&) {
    }

    try {
        const auto normalized = NormalizeZmiVendorBatteryHint(winrt::unbox_value<std::int32_t>(raw_value));
        if (!normalized.has_value()) {
            DebugLog("ZMI vendor key was present but value was not usable (int32)");
        }
        return normalized;
    } catch (const winrt::hresult_error&) {
    }

    DebugLog("ZMI vendor key type was not supported");
    return std::nullopt;
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

    auto requested_properties = winrt::single_threaded_vector<winrt::hstring>();
    requested_properties.Append(L"System.ItemNameDisplay");
    requested_properties.Append(L"System.Devices.Aep.DeviceAddress");
    requested_properties.Append(L"System.Devices.BatteryLife");
    requested_properties.Append(L"System.Devices.BatteryPlusCharging");
    requested_properties.Append(L"System.Devices.BatteryPlusChargingText");
    requested_properties.Append(L"{670245F9-6E25-4179-85C1-981C33B9D3B7} 4");

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
            candidate.from_connected_scan = true;
            tws_candidates->push_back(std::move(candidate));
        }

        auto battery_percent = ReadBatteryPercentFromEndpointProperties(device_info);
        if (!battery_percent.has_value() && IsLikelyZmiPurPods(device_name, device_name, device_id)) {
            battery_percent = ReadZmiVendorBatteryHint(device_info);
            if (!battery_percent.has_value() && address.has_value()) {
                battery_percent = ReadZmiVendorBatteryHintFromPnpAddress(*address);
                if (battery_percent.has_value()) {
                    DebugLog("Fast connected fallback: ZMI PnP vendor key battery=" + std::to_string(*battery_percent));
                }
            }
            if (battery_percent.has_value()) {
                DebugLog("Fast connected fallback: ZMI vendor key battery=" + std::to_string(*battery_percent));
            }
        }
        DeviceBatteryInfo entry;
        entry.device_id = device_id;
        entry.device_name = device_name.empty() ? "Unknown" : device_name;
        entry.battery_component = NormalizeComponentHint(device_name);
        if (entry.battery_component.empty()) {
            entry.battery_component = "main";
        }
        entry.battery_level_percent = battery_percent;
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
                    candidate.from_connected_scan = false;
                    tws_candidates->push_back(std::move(candidate));
                }

                auto battery_percent = ReadBatteryPercentFromEndpointProperties(device_info);
                if (!battery_percent.has_value() && IsLikelyZmiPurPods(device_name, device_name, device_id)) {
                    battery_percent = ReadZmiVendorBatteryHint(device_info);
                    if (!battery_percent.has_value() && address.has_value()) {
                        battery_percent = ReadZmiVendorBatteryHintFromPnpAddress(*address);
                    }
                }

                DeviceBatteryInfo entry;
                entry.device_id = device_id;
                entry.device_name = device_name.empty() ? "Unknown" : device_name;
                entry.battery_component = NormalizeComponentHint(device_name);
                if (entry.battery_component.empty()) {
                    entry.battery_component = "main";
                }
                entry.battery_level_percent = battery_percent;
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
    requested_properties.Append(L"{670245F9-6E25-4179-85C1-981C33B9D3B7} 4");

    constexpr auto kEndpointSelector = LR"((System.Devices.Aep.IsPresent:=System.StructuredQueryType.Boolean#True)
AND ((System.Devices.Aep.ProtocolId:="{E0CBF06C-CD8B-4647-BB8A-263B43F0F974}")
OR (System.Devices.Aep.ProtocolId:="{BB7BB05E-5972-42B5-94FC-76EAA7084D49}")))";

    const auto query_started_at = std::chrono::steady_clock::now();
    const auto endpoint_infos_operation =
        DeviceInformation::FindAllAsync(kEndpointSelector, requested_properties, DeviceInformationKind::AssociationEndpoint);
    const auto endpoint_infos_result =
        WaitForAsyncResult(endpoint_infos_operation, std::chrono::milliseconds(1800));
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
                    candidate.from_connected_scan = false;
                    tws_candidates->push_back(std::move(candidate));
                    if (interesting) {
                        DebugLog("AEP tws candidate address: " + address_key);
                    }
                }
            } else if (interesting) {
                DebugLog("AEP tws candidate has no parseable DeviceAddress");
            }
        }

        auto battery_percent = ReadBatteryPercentFromEndpointProperties(endpoint_info);
        if (!battery_percent.has_value() && IsLikelyZmiPurPods(endpoint_name, endpoint_name, endpoint_id)) {
            battery_percent = ReadZmiVendorBatteryHint(endpoint_info);
            if (!battery_percent.has_value()) {
                const auto parsed_address = ParseBluetoothAddressFromDeviceId(endpoint_id);
                if (parsed_address.has_value()) {
                    battery_percent = ReadZmiVendorBatteryHintFromPnpAddress(*parsed_address);
                    if (battery_percent.has_value()) {
                        DebugLog("AEP fallback: ZMI PnP vendor key battery=" + std::to_string(*battery_percent));
                    }
                }
            }
            if (battery_percent.has_value()) {
                DebugLog("AEP fallback: ZMI vendor key battery=" + std::to_string(*battery_percent));
            }
        }
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
    requested_properties.Append(L"System.Devices.BatteryLife");
    requested_properties.Append(L"System.Devices.BatteryPlusCharging");
    requested_properties.Append(L"System.Devices.BatteryPlusChargingText");
    requested_properties.Append(L"System.ItemNameDisplay");
    requested_properties.Append(L"{670245F9-6E25-4179-85C1-981C33B9D3B7} 4");

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

        auto battery = ReadBatteryPercentFromEndpointProperties(device);
        if (!battery.has_value() && IsLikelyZmiPurPods(device_name_probe, device_name_probe, device_id)) {
            battery = ReadZmiVendorBatteryHint(device);
            if (!battery.has_value()) {
                const auto parsed_address = ParseBluetoothAddressFromDeviceId(device_id);
                if (parsed_address.has_value()) {
                    battery = ReadZmiVendorBatteryHintFromPnpAddress(*parsed_address);
                    if (battery.has_value()) {
                        DebugLog("Generic fallback: ZMI PnP vendor key battery=" + std::to_string(*battery));
                    }
                }
            }
            if (battery.has_value()) {
                DebugLog("Generic fallback: ZMI vendor key battery=" + std::to_string(*battery));
            }
        }
        if (!battery.has_value()) {
            if (interesting) {
                DebugLog("Generic battery props not found for name='" + device_name_probe + "' id='" + device_id + "'");
            }
            continue;
        }

        std::string name = ToUtf8(device.Name());
        if (name.empty()) {
            TryGetStringProperty(device, L"System.ItemNameDisplay", &name);
        }
        if (name.empty()) {
            name = "Unknown";
        }

        DeviceBatteryInfo entry;
        entry.device_id = device_id;
        entry.device_name = name;
        entry.battery_component = NormalizeComponentHint(name);
        if (entry.battery_component.empty()) {
            entry.battery_component = "main";
        }
        entry.battery_level_percent = *battery;
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
        const auto maybe_device_infos =
            WaitForAsyncResult(DeviceInformation::FindAllAsync(selector), std::chrono::milliseconds(1800));
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
    if (candidates.empty()) {
        try {
            AddCandidatesFromSelector(BluetoothLEDevice::GetDeviceSelectorFromPairingState(true), &candidates, &known_ids);
        } catch (const winrt::hresult_error& error) {
            DebugLog("GetDeviceSelectorFromPairingState failed: " + DescribeHresultError(error));
        }
    }

    return candidates;
}

}  // namespace

std::vector<DeviceBatteryInfo> WinRtBatteryProvider::GetConnectedDevicesBattery() {
    EnsureApartmentInitialized();
    try {

        std::vector<DeviceBatteryInfo> devices_with_battery;
        std::unordered_set<std::uint64_t> addresses_with_real_battery;
        std::unordered_map<std::uint64_t, XiaomiReadResult> xiaomi_classic_cache;
        std::unordered_set<std::string> known_entries;
        auto try_add_entry = [&](DeviceBatteryInfo entry) {
            const std::string dedupe_key = MakeEntryKey(entry);
            if (!known_entries.insert(dedupe_key).second) {
                return;
            }
            if (entry.battery_level_percent.has_value() && !entry.is_cached) {
                const auto parsed_address = ParseBluetoothAddressFromDeviceId(entry.device_id);
                if (parsed_address.has_value()) {
                    addresses_with_real_battery.insert(*parsed_address);
                }
            }
            devices_with_battery.push_back(std::move(entry));
        };
        auto read_xiaomi_classic_cached = [&](std::uint64_t address, bool aggressive_retry) -> const XiaomiReadResult& {
            if (const auto found = xiaomi_classic_cache.find(address); found != xiaomi_classic_cache.end()) {
                return found->second;
            }

            XiaomiReadResult read_result;
            auto readings = TryReadXiaomiClassicBattery(address);
            if (aggressive_retry && readings.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(180));
                readings = TryReadXiaomiClassicBattery(address);
            }
            if (aggressive_retry && readings.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(260));
                readings = TryReadXiaomiClassicBattery(address);
            }

            if (!readings.empty()) {
                read_result.readings = readings;
                read_result.from_persistent_cache = false;
                PutPersistentXiaomiSnapshot(address, SnapshotFromBatteryReadings(readings));
            } else {
                const auto persistent_snapshot = GetPersistentXiaomiSnapshot(address);
                if (persistent_snapshot.has_value()) {
                    read_result.readings = BuildXiaomiBatteryReadings(*persistent_snapshot);
                    read_result.from_persistent_cache = true;
                    DebugLog("Xiaomi classic fallback: using persisted cache for address=" + std::to_string(address));
                }
            }

            auto inserted = xiaomi_classic_cache.emplace(address, std::move(read_result));
            return inserted.first->second;
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
            if (!likely_xiaomi_tws) {
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
            if (likely_xiaomi_tws && resolved_readings.size() <= 1U) {
                std::optional<std::uint64_t> classic_address = TryGetBluetoothAddress(ble_device);
                if (!classic_address.has_value()) {
                    classic_address = ParseBluetoothAddressFromDeviceId(device_id);
                }

                if (classic_address.has_value()) {
                    const auto& classic_result =
                        read_xiaomi_classic_cached(*classic_address, aggressive_xiaomi_retry);
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

                if (likely_tws) {
                    DeviceBatteryInfo unknown_entry;
                    unknown_entry.device_id = device_id;
                    unknown_entry.device_name = device_name;
                    unknown_entry.battery_component = "main";
                    unknown_entry.battery_level_percent = std::nullopt;
                    try_add_entry(std::move(unknown_entry));
                }
                continue;
            }

            for (const auto& battery_reading : resolved_readings) {
                DeviceBatteryInfo entry;
                entry.device_id = device_id;
                entry.device_name = device_name;
                entry.battery_component = battery_reading.component;
                entry.battery_level_percent = battery_reading.percent;
                entry.is_cached = resolved_from_persistent_cache;
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
                const bool already_known =
                    std::any_of(tws_candidates.begin(), tws_candidates.end(),
                                [&candidate](const EndpointCandidate& existing) {
                                    return existing.bluetooth_address == candidate.bluetooth_address;
                                });
                if (!already_known) {
                    tws_candidates.push_back(candidate);
                }
            }
        } else {
            DebugLog("AEP scan skipped because fast candidate scan already found targets.");
        }

        for (const auto& candidate : tws_candidates) {
            if (addresses_with_real_battery.contains(candidate.bluetooth_address)) {
                continue;
            }

            const bool likely_xiaomi_tws = IsLikelyXiaomiEarbuds(candidate.endpoint_name, candidate.endpoint_name, candidate.endpoint_id);
            const bool aggressive_xiaomi_retry =
                candidate.from_connected_scan &&
                ShouldAggressiveXiaomiClassicRetry(candidate.endpoint_name, candidate.endpoint_name, candidate.endpoint_id);
            if (likely_xiaomi_tws) {
                const auto& classic_result =
                    read_xiaomi_classic_cached(candidate.bluetooth_address, aggressive_xiaomi_retry);
                if (!classic_result.readings.empty()) {
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
                        try_add_entry(std::move(entry));
                    }
                    continue;
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

                DeviceBatteryInfo unknown_entry;
                unknown_entry.device_id =
                    candidate.endpoint_id.empty() ? ("BluetoothAddress#" + std::to_string(candidate.bluetooth_address))
                                                  : candidate.endpoint_id;
                unknown_entry.device_name = candidate.endpoint_name.empty() ? "Unknown" : candidate.endpoint_name;
                unknown_entry.battery_component = "main";
                unknown_entry.battery_level_percent = std::nullopt;
                try_add_entry(std::move(unknown_entry));
                continue;
            }
            const auto ble_device = *maybe_ble_device;

            std::vector<BatteryReading> resolved_readings;
            if (!likely_xiaomi_tws) {
                resolved_readings = ReadBatteryReadings(ble_device, true);
            } else {
                DebugLog("AEP candidate: Xiaomi-family TWS, skip standard battery service read");
            }
            bool resolved_from_persistent_cache = false;
            if (likely_xiaomi_tws && resolved_readings.size() <= 1U) {
                const auto& classic_result =
                    read_xiaomi_classic_cached(candidate.bluetooth_address, aggressive_xiaomi_retry);
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
                try_add_entry(std::move(unknown_entry));
                continue;
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
                entry.is_cached = resolved_from_persistent_cache;
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

    std::unordered_set<std::string> devices_with_real_battery;
    std::unordered_set<std::string> devices_with_live_battery;
    std::unordered_set<std::uint64_t> addresses_with_live_battery;
    std::unordered_set<std::uint64_t> addresses_with_any_real_battery;
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
    }

    std::vector<DeviceBatteryInfo> filtered_entries;
    filtered_entries.reserve(devices_with_battery.size());
    std::unordered_set<std::string> final_dedup;
    for (auto& entry : devices_with_battery) {
        const auto parsed_address = ParseBluetoothAddressFromDeviceId(entry.device_id);

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
            key = MakeEntryKey(entry);
        }
        if (!final_dedup.insert(key).second) {
            continue;
        }

        filtered_entries.push_back(std::move(entry));
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

}  // namespace battery_monitor
