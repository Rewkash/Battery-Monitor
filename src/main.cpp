#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "core/BatteryTypes.h"
#include "core/ProviderFactory.h"

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
            default:
                escaped << ch;
                break;
        }
    }
    return escaped.str();
}

void PrintTable(const std::vector<DeviceBatteryInfo>& devices) {
    if (devices.empty()) {
        std::cout << "No connected Bluetooth devices with battery information were found.\n";
        return;
    }

    std::cout << "Device Name | Component | Battery | Device ID\n";
    std::cout << "-------------------------------------------------------------\n";

    for (const auto& device : devices) {
        const std::string battery_text =
            device.battery_level_percent.has_value() ? (std::to_string(*device.battery_level_percent) + "%") : "N/A";
        std::cout << device.device_name << " | " << device.battery_component << " | "
                  << battery_text << " | " << device.device_id << '\n';
    }
}

void PrintJson(const std::vector<DeviceBatteryInfo>& devices) {
    std::cout << "[\n";
    for (std::size_t i = 0; i < devices.size(); ++i) {
        const auto& device = devices[i];
        const std::string battery_json =
            device.battery_level_percent.has_value() ? std::to_string(*device.battery_level_percent) : "null";
        std::cout << "  {\"deviceId\":\"" << EscapeJson(device.device_id) << "\","
                  << "\"deviceName\":\"" << EscapeJson(device.device_name) << "\","
                  << "\"component\":\"" << EscapeJson(device.battery_component) << "\","
                  << "\"batteryLevelPercent\":" << battery_json << "}";
        if (i + 1 < devices.size()) {
            std::cout << ",";
        }
        std::cout << '\n';
    }
    std::cout << "]\n";
}

}  // namespace

}  // namespace battery_monitor

int main(int argc, char** argv) {
    bool json_output = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--json") {
            json_output = true;
        }
    }

    try {
        auto provider = battery_monitor::CreateBatteryProvider();
        const auto devices = provider->GetConnectedDevicesBattery();

        if (json_output) {
            battery_monitor::PrintJson(devices);
        } else {
            battery_monitor::PrintTable(devices);
        }

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n';
        return 1;
    }
}
