#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifdef BATTERY_MONITOR_WITH_QT
#include <QApplication>

#include "ui/BatteryWindow.h"
#endif

#ifdef _WIN32
#include <winrt/base.h>
#endif

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
        std::string battery_text =
            device.battery_level_percent.has_value() ? (std::to_string(*device.battery_level_percent) + "%") : "N/A";
        if (device.is_cached && device.battery_level_percent.has_value()) {
            battery_text += " (cached)";
        }
        if (!device.is_connected) {
            battery_text += " (offline)";
        }
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
                  << "\"batteryLevelPercent\":" << battery_json << ","
                  << "\"isCached\":" << (device.is_cached ? "true" : "false") << ","
                  << "\"isConnected\":" << (device.is_connected ? "true" : "false") << "}";
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
    bool cli_output = false;
    bool gui_forced = false;
    bool include_offline = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json") {
            json_output = true;
            continue;
        }
        if (arg == "--cli") {
            cli_output = true;
            continue;
        }
        if (arg == "--gui") {
            gui_forced = true;
            continue;
        }
        if (arg == "--all" || arg == "--include-offline") {
            include_offline = true;
        }
    }

    try {
#ifdef BATTERY_MONITOR_WITH_QT
        if (!json_output && (!cli_output || gui_forced)) {
            QApplication app(argc, argv);
            auto provider = battery_monitor::CreateBatteryProvider();
            battery_monitor::BatteryWindow window(std::move(provider));
            window.Launch();
            return app.exec();
        }
#endif
        auto provider = battery_monitor::CreateBatteryProvider();
        battery_monitor::BatteryQueryOptions query_options;
        query_options.include_disconnected = include_offline;
        const auto devices = provider->GetDevicesBattery(query_options);

        if (json_output) {
            battery_monitor::PrintJson(devices);
        } else {
            battery_monitor::PrintTable(devices);
        }

        return 0;
#ifdef _WIN32
    } catch (const winrt::hresult_error& ex) {
        std::cerr << "Error: WinRT HRESULT=0x" << std::hex << std::uppercase
                  << static_cast<std::uint32_t>(ex.code().value)
                  << " message=" << winrt::to_string(ex.message()) << '\n';
        return 1;
#endif
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Error: unknown exception\n";
        return 1;
    }
}
