#include "platform/windows/devices/controller/WindowsControllerBatteryReader.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <exception>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <winrt/Windows.Devices.Power.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/base.h>

namespace battery_monitor {

namespace {

using winrt::Windows::Devices::Power::BatteryReport;
using winrt::Windows::Gaming::Input::Gamepad;
using winrt::Windows::Gaming::Input::IGameControllerBatteryInfo;
using winrt::Windows::Gaming::Input::RawGameController;

struct DualShockHidRouteCacheEntry {
    std::wstring path;
    std::wstring instance_id;
};

struct ControllerBatteryCacheEntry {
    std::optional<std::uint8_t> value;
    std::chrono::steady_clock::time_point captured_at = std::chrono::steady_clock::now();
};

void LogDebug(bool debug_enabled, ControllerDebugLogFn debug_log, const std::string& message) {
    if (debug_enabled && debug_log != nullptr) {
        debug_log(message);
    }
}

std::string ToUtf8(const std::wstring& value) {
    return winrt::to_string(winrt::hstring(value));
}

std::string ToUtf8(const winrt::hstring& value) {
    return winrt::to_string(value);
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
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

bool IsLikelySonyDualShockController(const std::string& primary_name,
                                     const std::string& secondary_name,
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

std::optional<std::uint8_t> TryConvertBatteryReportToPercent(const BatteryReport& report) {
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

std::optional<std::uint64_t> ParseBluetoothAddress(const std::string& value) {
    if (value.empty()) {
        return std::nullopt;
    }

    std::string compact;
    compact.reserve(value.size());
    for (const char ch : value) {
        if (ch == ':' || ch == '-' || ch == ' ') {
            continue;
        }
        compact.push_back(ch);
    }

    if (compact.size() != 12U) {
        return std::nullopt;
    }

    for (const char ch : compact) {
        const bool is_digit = (ch >= '0' && ch <= '9');
        const bool is_lower_hex = (ch >= 'a' && ch <= 'f');
        const bool is_upper_hex = (ch >= 'A' && ch <= 'F');
        if (!is_digit && !is_lower_hex && !is_upper_hex) {
            return std::nullopt;
        }
    }

    try {
        return std::stoull(compact, nullptr, 16);
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

    for (std::size_t index = 0; index < lowered.size(); ++index) {
        if (index + 17 <= lowered.size() && lowered[index + 2] == ':' && lowered[index + 5] == ':' &&
            lowered[index + 8] == ':' && lowered[index + 11] == ':' && lowered[index + 14] == ':') {
            const auto parsed = try_parse_window(index);
            if (parsed.has_value() &&
                std::find(addresses.begin(), addresses.end(), *parsed) == addresses.end()) {
                addresses.push_back(*parsed);
            }
        }
    }

    for (std::size_t index = 0; index + 16 <= lowered.size(); ++index) {
        if (lowered.compare(index, 4, "dev_") != 0) {
            continue;
        }

        const std::string candidate = lowered.substr(index + 4, 12);
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
    return addresses.back();
}

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

int ControllerBatteryCacheTtlMs() {
    static const int ttl_ms = []() {
        char* value = nullptr;
        std::size_t length = 0;
        const errno_t status = _dupenv_s(&value, &length, "BATTERY_MONITOR_CONTROLLER_CACHE_MS");
        int parsed = 45000;
        if (status == 0 && value != nullptr) {
            try {
                parsed = std::stoi(value);
            } catch (const std::exception&) {
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
        return static_cast<std::uint8_t>(battery_data * 10U + 5U);
    }
    if (battery_data == 10U) {
        return static_cast<std::uint8_t>(100U);
    }
    if (cable_connected && battery_data == 11U) {
        return static_cast<std::uint8_t>(100U);
    }
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
                                                           const std::string& device_id_hint,
                                                           bool debug_enabled,
                                                           ControllerDebugLogFn debug_log) {
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
        if (!HidD_GetAttributes(device_handle, &attributes) || attributes.VendorID != 0x054CU) {
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
        for (const auto report_id : std::array<std::uint8_t, 2>{0x11U, 0x01U}) {
            std::fill(report.begin(), report.end(), std::uint8_t{0U});
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
                std::fill(report.begin(), report.end(), std::uint8_t{0U});
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
            LogDebug(debug_enabled, debug_log,
                     "DualShock HID fallback: no input report for instance='" + ToUtf8(instance_id) + "'");
            return std::nullopt;
        }

        std::size_t used_offset = 0;
        const auto parsed = ParseDualShockBatteryFromInputReport(report, &used_offset);
        if (!parsed.has_value()) {
            LogDebug(debug_enabled, debug_log,
                     "DualShock HID fallback: report parsed but battery offset was not recognized for instance='" +
                         ToUtf8(instance_id) + "'");
            return std::nullopt;
        }

        LogDebug(debug_enabled, debug_log,
                 "DualShock HID battery fallback accepted value=" + std::to_string(*parsed) +
                     " reportId=0x" + BytesToHex(std::vector<std::uint8_t>{report[0]}) +
                     " offset=" + std::to_string(used_offset) +
                     " instance='" + ToUtf8(instance_id) + "'");
        return parsed;
    };

    if (const auto cached_route = TryGetDualShockHidRoute(cache_key); cached_route.has_value()) {
        LogDebug(debug_enabled, debug_log,
                 "DualShock HID fallback: trying cached route for key='" + cache_key + "'");
        if (const auto cached_reading = try_read_candidate(cached_route->path, cached_route->instance_id);
            cached_reading.has_value()) {
            return cached_reading;
        }
        LogDebug(debug_enabled, debug_log,
                 "DualShock HID fallback: cached route failed, rescanning candidates for key='" + cache_key + "'");
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

        candidates.push_back(HidCandidate{detail->DevicePath, std::move(instance_id), score});
    }

    std::sort(candidates.begin(), candidates.end(), [](const HidCandidate& lhs, const HidCandidate& rhs) {
        return lhs.score > rhs.score;
    });
    LogDebug(debug_enabled, debug_log,
             "DualShock HID fallback candidates: " + std::to_string(candidates.size()));

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
                                                                  const std::string& device_id_hint,
                                                                  bool debug_enabled,
                                                                  ControllerDebugLogFn debug_log) {
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

            LogDebug(debug_enabled, debug_log,
                     "GameInput controller battery accepted for '" + device_name_hint +
                         "' (raw='" + candidate_name + "', idHint='" + device_id_hint +
                         "') value=" + std::to_string(*percent));

            if (has_specific_hint) {
                return percent;
            }
            if (!best_fallback.has_value()) {
                best_fallback = percent;
            }
        }

        if (!best_fallback.has_value() && !has_specific_hint) {
            for (const auto& gamepad : Gamepad::Gamepads()) {
                const auto battery_info = gamepad.try_as<IGameControllerBatteryInfo>();
                if (!battery_info) {
                    continue;
                }
                const auto battery_report = battery_info.TryGetBatteryReport();
                const auto percent = TryConvertBatteryReportToPercent(battery_report);
                if (!percent.has_value()) {
                    continue;
                }
                LogDebug(debug_enabled, debug_log,
                         "GameInput gamepad battery fallback accepted for '" + device_name_hint +
                             "' (idHint='" + device_id_hint + "') value=" + std::to_string(*percent));
                best_fallback = percent;
                break;
            }
        }
    } catch (const winrt::hresult_error& error) {
        LogDebug(debug_enabled, debug_log,
                 "GameInput controller battery fallback failed: " + DescribeHresultError(error));
    }

    return best_fallback;
}

}  // namespace

std::optional<std::uint8_t> ReadControllerBatteryCached(const std::string& device_name_hint,
                                                        const std::string& device_id_hint,
                                                        bool debug_enabled,
                                                        ControllerDebugLogFn debug_log) {
    std::string cache_key = NormalizeControllerNameForMatch(device_name_hint);
    if (cache_key.empty()) {
        cache_key = ToLowerAscii(device_id_hint);
    }
    if (cache_key.empty()) {
        return std::nullopt;
    }

    const int controller_cache_ttl_ms = ControllerBatteryCacheTtlMs();
    const auto persistent_cache_entry = TryGetControllerBatteryCacheEntry(cache_key);
    if (persistent_cache_entry.has_value() && controller_cache_ttl_ms > 0) {
        const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - persistent_cache_entry->captured_at);
        if (age_ms.count() <= controller_cache_ttl_ms) {
            LogDebug(debug_enabled, debug_log,
                     "Controller battery cache hit key='" + cache_key +
                         "' ageMs=" + std::to_string(age_ms.count()));
            return persistent_cache_entry->value;
        }
    }

    const auto game_input_read =
        TryReadControllerBatteryFromGameInput(device_name_hint, device_id_hint, debug_enabled, debug_log);
    if (!game_input_read.has_value() &&
        IsLikelySonyDualShockController(device_name_hint, device_name_hint, device_id_hint)) {
        const auto hid_read =
            TryReadDualShockBatteryFromHid(device_name_hint, device_id_hint, debug_enabled, debug_log);
        if (hid_read.has_value()) {
            PutControllerBatteryCacheEntry(cache_key, hid_read);
            return hid_read;
        }
    }

    if (!game_input_read.has_value() &&
        persistent_cache_entry.has_value() &&
        persistent_cache_entry->value.has_value()) {
        const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - persistent_cache_entry->captured_at);
        LogDebug(debug_enabled, debug_log,
                 "Controller battery cache stale fallback key='" + cache_key +
                     "' ageMs=" + std::to_string(age_ms.count()));
        return persistent_cache_entry->value;
    }

    if (!game_input_read.has_value()) {
        LogDebug(debug_enabled, debug_log,
                 "GameInput controller battery fallback: no battery report for '" +
                     device_name_hint + "' id='" + device_id_hint + "'");
    }

    PutControllerBatteryCacheEntry(cache_key, game_input_read);
    return game_input_read;
}

}  // namespace battery_monitor

