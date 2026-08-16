#include "app/BatteryOutputFormatter.h"

#include <iomanip>
#include <sstream>
#include <string>

namespace battery_monitor {

namespace {

std::string EscapeJson(const std::string& input) {
    std::ostringstream escaped;
    for (const char ch : input) {
        switch (ch) {
            case '\\':
                escaped << "\\\\";
                break;
            case '\"':
                escaped << "\\\"";
                break;
            case '\b':
                escaped << "\\b";
                break;
            case '\f':
                escaped << "\\f";
                break;
            case '\n':
                escaped << "\\n";
                break;
            case '\r':
                escaped << "\\r";
                break;
            case '\t':
                escaped << "\\t";
                break;
            default: {
                const auto byte = static_cast<unsigned char>(ch);
                // Remaining control characters must not appear raw in JSON
                // strings; UTF-8 continuation bytes (>= 0x80) pass through.
                if (byte < 0x20) {
                    escaped << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                            << static_cast<int>(byte) << std::dec;
                } else {
                    escaped << ch;
                }
                break;
            }
        }
    }
    return escaped.str();
}

std::string FormatBatteryText(const DeviceBatteryInfo& device) {
    std::string battery_text =
        device.battery_level_percent.has_value() ? (std::to_string(*device.battery_level_percent) + "%") : "N/A";
    if (device.is_cached && device.battery_level_percent.has_value()) {
        battery_text += " (cached)";
    }
    if (!device.is_connected) {
        battery_text += " (offline)";
    }
    if (device.device_mode.has_value()) {
        battery_text += " [" + *device.device_mode + "]";
        if (device.device_submode.has_value()) {
            battery_text += "{" + *device.device_submode + "}";
        }
    }

    return battery_text;
}

void PrintTable(const std::vector<DeviceBatteryInfo>& devices, std::ostream& stream) {
    if (devices.empty()) {
        stream << "No connected Bluetooth devices with battery information were found.\n";
        return;
    }

    stream << "Device Name | Component | Battery | Device ID\n";
    stream << "-------------------------------------------------------------\n";

    for (const auto& device : devices) {
        stream << device.device_name << " | " << device.battery_component << " | " << FormatBatteryText(device)
               << " | " << device.device_id << '\n';
    }
}

void PrintJson(const std::vector<DeviceBatteryInfo>& devices, std::ostream& stream) {
    stream << "[\n";
    for (std::size_t i = 0; i < devices.size(); ++i) {
        const auto& device = devices[i];
        const std::string battery_json =
            device.battery_level_percent.has_value() ? std::to_string(*device.battery_level_percent) : "null";
        stream << "  {\"deviceId\":\"" << EscapeJson(device.device_id) << "\","
               << "\"deviceName\":\"" << EscapeJson(device.device_name) << "\","
               << "\"component\":\"" << EscapeJson(device.battery_component) << "\","
               << "\"batteryLevelPercent\":" << battery_json << ","
               << "\"deviceMode\":"
               << (device.device_mode.has_value() ? ("\"" + EscapeJson(*device.device_mode) + "\"") : "null") << ","
               << "\"deviceSubmode\":"
               << (device.device_submode.has_value() ? ("\"" + EscapeJson(*device.device_submode) + "\"") : "null")
               << ","
               << "\"isCached\":" << (device.is_cached ? "true" : "false") << ","
               << "\"isConnected\":" << (device.is_connected ? "true" : "false") << "}";
        if (i + 1 < devices.size()) {
            stream << ",";
        }
        stream << '\n';
    }
    stream << "]\n";
}

}  // namespace

void PrintDevices(const std::vector<DeviceBatteryInfo>& devices, DeviceListOutputFormat format, std::ostream& stream) {
    if (format == DeviceListOutputFormat::Json) {
        PrintJson(devices, stream);
        return;
    }

    PrintTable(devices, stream);
}

}  // namespace battery_monitor
