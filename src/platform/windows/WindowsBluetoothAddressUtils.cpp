#include "platform/windows/WindowsBluetoothAddressUtils.h"

#include <algorithm>
#include <exception>
#include <iomanip>
#include <sstream>

#include <windows.h>

namespace battery_monitor {

namespace {

std::string ToLowerAscii(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

}  // namespace

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
            if (parsed.has_value() && std::find(addresses.begin(), addresses.end(), *parsed) == addresses.end()) {
                addresses.push_back(*parsed);
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

std::optional<std::uint64_t> TryGetBluetoothAddress(
    const winrt::Windows::Devices::Bluetooth::BluetoothLEDevice& ble_device) {
    try {
        const auto address = ble_device.BluetoothAddress();
        if (address > 0xFFFFULL) {
            return address;
        }
    } catch (const winrt::hresult_error&) {
    }

    return std::nullopt;
}

}  // namespace battery_monitor
