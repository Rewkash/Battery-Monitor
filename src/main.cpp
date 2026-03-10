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

#include "platform/windows/WinRtBatteryProvider.h"
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
        if (device.device_mode.has_value()) {
            battery_text += " [" + *device.device_mode + "]";
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
                  << "\"deviceMode\":"
                  << (device.device_mode.has_value() ? ("\"" + EscapeJson(*device.device_mode) + "\"") : "null") << ","
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
    bool probe_xiaomi_noise = false;
    bool observe_xiaomi_control = false;
    bool observe_zmi_serial = false;
    bool dump_bluetooth_services = false;
    bool dump_ble_gatt = false;
    bool set_xiaomi_noise_mode = false;
    bool send_xiaomi_candidate = false;
    bool set_xiaomi_submode = false;
    std::string probe_device_hint;
    std::string requested_noise_mode;
    std::string requested_submode_family;
    int requested_candidate_id = 0;
    int requested_submode = 0;
    int observe_seconds = 45;
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
            continue;
        }
        if (arg == "--probe-xiaomi-noise") {
            probe_xiaomi_noise = true;
            if (i + 1 < argc) {
                const std::string next = argv[i + 1];
                if (!next.empty() && next[0] != '-') {
                    probe_device_hint = next;
                    ++i;
                }
            }
            continue;
        }
        if (arg == "--observe-xiaomi-control") {
            observe_xiaomi_control = true;
            if (i + 1 < argc) {
                const std::string next = argv[i + 1];
                if (!next.empty() && next[0] != '-') {
                    probe_device_hint = next;
                    ++i;
                }
            }
            continue;
        }
        if (arg == "--observe-zmi-serial") {
            observe_zmi_serial = true;
            if (i + 1 < argc) {
                const std::string next = argv[i + 1];
                if (!next.empty() && next[0] != '-') {
                    probe_device_hint = next;
                    ++i;
                }
            }
            continue;
        }
        if (arg == "--dump-bt-services") {
            dump_bluetooth_services = true;
            if (i + 1 < argc) {
                const std::string next = argv[i + 1];
                if (!next.empty() && next[0] != '-') {
                    probe_device_hint = next;
                    ++i;
                }
            }
            continue;
        }
        if (arg == "--dump-ble-gatt") {
            dump_ble_gatt = true;
            if (i + 1 < argc) {
                const std::string next = argv[i + 1];
                if (!next.empty() && next[0] != '-') {
                    probe_device_hint = next;
                    ++i;
                }
            }
            continue;
        }
        if (arg == "--observe-seconds" && i + 1 < argc) {
            observe_seconds = std::max(5, std::stoi(argv[++i]));
            continue;
        }
        if (arg == "--xiaomi-set-noise" && i + 1 < argc) {
            set_xiaomi_noise_mode = true;
            requested_noise_mode = argv[++i];
            if (i + 1 < argc) {
                const std::string next = argv[i + 1];
                if (!next.empty() && next[0] != '-') {
                    probe_device_hint = next;
                    ++i;
                }
            }
            continue;
        }
        if (arg == "--xiaomi-test-candidate" && i + 1 < argc) {
            send_xiaomi_candidate = true;
            requested_candidate_id = std::stoi(argv[++i]);
            if (i + 1 < argc) {
                const std::string next = argv[i + 1];
                if (!next.empty() && next[0] != '-') {
                    probe_device_hint = next;
                    ++i;
                }
            }
            continue;
        }
        if (arg == "--xiaomi-set-submode" && i + 2 < argc) {
            set_xiaomi_submode = true;
            requested_submode_family = argv[++i];
            requested_submode = std::stoi(argv[++i]);
            if (i + 1 < argc) {
                const std::string next = argv[i + 1];
                if (!next.empty() && next[0] != '-') {
                    probe_device_hint = next;
                    ++i;
                }
            }
            continue;
        }
    }

    try {
#ifdef _WIN32
        if (set_xiaomi_submode) {
            battery_monitor::WinRtBatteryProvider provider;
            return provider.SetXiaomiNoiseSubmode(requested_submode_family, requested_submode, probe_device_hint) ? 0 : 2;
        }
        if (send_xiaomi_candidate) {
            battery_monitor::WinRtBatteryProvider provider;
            return provider.SendXiaomiControlCandidate(requested_candidate_id, probe_device_hint) ? 0 : 2;
        }
        if (set_xiaomi_noise_mode) {
            battery_monitor::WinRtBatteryProvider provider;
            return provider.SetXiaomiNoiseMode(requested_noise_mode, probe_device_hint) ? 0 : 2;
        }
        if (observe_xiaomi_control) {
            battery_monitor::WinRtBatteryProvider provider;
            return provider.ObserveXiaomiControlSession(probe_device_hint, observe_seconds) ? 0 : 2;
        }
        if (observe_zmi_serial) {
            battery_monitor::WinRtBatteryProvider provider;
            return provider.ObserveZmiSerialSession(probe_device_hint, observe_seconds) ? 0 : 2;
        }
        if (dump_bluetooth_services) {
            battery_monitor::WinRtBatteryProvider provider;
            return provider.DumpBluetoothServices(probe_device_hint) ? 0 : 2;
        }
        if (dump_ble_gatt) {
            battery_monitor::WinRtBatteryProvider provider;
            return provider.DumpBleGatt(probe_device_hint) ? 0 : 2;
        }
        if (probe_xiaomi_noise) {
            battery_monitor::WinRtBatteryProvider provider;
            return provider.ProbeXiaomiNoiseControl(probe_device_hint) ? 0 : 2;
        }
#endif
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
