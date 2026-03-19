#include "app/CommandLineOptions.h"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace battery_monitor {

namespace {

bool TryConsumeOptionalValue(int argc, char** argv, int& index, std::string& value) {
    if (index + 1 >= argc) {
        return false;
    }

    const std::string next = argv[index + 1];
    if (next.empty() || next[0] == '-') {
        return false;
    }

    value = next;
    ++index;
    return true;
}

}  // namespace

bool PlatformCommandOptions::HasAnyCommand() const noexcept {
    return probe_xiaomi_noise || observe_xiaomi_control || observe_zmi_serial || dump_bluetooth_services ||
           dump_ble_gatt || set_xiaomi_noise_mode || send_xiaomi_candidate || set_xiaomi_submode;
}

CommandLineOptions ParseCommandLine(int argc, char** argv) {
    CommandLineOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json") {
            options.json_output = true;
            continue;
        }
        if (arg == "--cli") {
            options.cli_output = true;
            continue;
        }
        if (arg == "--gui") {
            options.gui_forced = true;
            continue;
        }
        if (arg == "--all" || arg == "--include-offline") {
            options.include_offline = true;
            continue;
        }
        if (arg == "--probe-xiaomi-noise") {
            options.platform_commands.probe_xiaomi_noise = true;
            TryConsumeOptionalValue(argc, argv, i, options.platform_commands.device_hint);
            continue;
        }
        if (arg == "--observe-xiaomi-control") {
            options.platform_commands.observe_xiaomi_control = true;
            TryConsumeOptionalValue(argc, argv, i, options.platform_commands.device_hint);
            continue;
        }
        if (arg == "--observe-zmi-serial") {
            options.platform_commands.observe_zmi_serial = true;
            TryConsumeOptionalValue(argc, argv, i, options.platform_commands.device_hint);
            continue;
        }
        if (arg == "--dump-bt-services") {
            options.platform_commands.dump_bluetooth_services = true;
            TryConsumeOptionalValue(argc, argv, i, options.platform_commands.device_hint);
            continue;
        }
        if (arg == "--dump-ble-gatt") {
            options.platform_commands.dump_ble_gatt = true;
            TryConsumeOptionalValue(argc, argv, i, options.platform_commands.device_hint);
            continue;
        }
        if (arg == "--observe-seconds" && i + 1 < argc) {
            options.platform_commands.observe_seconds = std::max(5, std::stoi(argv[++i]));
            continue;
        }
        if (arg == "--xiaomi-set-noise" && i + 1 < argc) {
            options.platform_commands.set_xiaomi_noise_mode = true;
            options.platform_commands.requested_noise_mode = argv[++i];
            TryConsumeOptionalValue(argc, argv, i, options.platform_commands.device_hint);
            continue;
        }
        if (arg == "--xiaomi-test-candidate" && i + 1 < argc) {
            options.platform_commands.send_xiaomi_candidate = true;
            options.platform_commands.requested_candidate_id = std::stoi(argv[++i]);
            TryConsumeOptionalValue(argc, argv, i, options.platform_commands.device_hint);
            continue;
        }
        if (arg == "--xiaomi-set-submode" && i + 2 < argc) {
            options.platform_commands.set_xiaomi_submode = true;
            options.platform_commands.requested_submode_family = argv[++i];
            options.platform_commands.requested_submode = std::stoi(argv[++i]);
            TryConsumeOptionalValue(argc, argv, i, options.platform_commands.device_hint);
            continue;
        }
    }

    return options;
}

bool ShouldLaunchGui(const CommandLineOptions& options, bool prefer_gui) noexcept {
    return !options.json_output && !options.cli_output && (prefer_gui || options.gui_forced);
}

}  // namespace battery_monitor
