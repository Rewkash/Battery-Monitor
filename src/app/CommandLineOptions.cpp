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
    return set_xiaomi_noise_mode || set_xiaomi_submode;
}

CommandLineOptions ParseCommandLine(int argc, char** argv) {
    CommandLineOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--version") {
            options.show_version = true;
            continue;
        }
        if (arg == "--check-updates") {
            options.check_updates = true;
            options.cli_output = true;
            continue;
        }
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
        if (arg == "--xiaomi-set-noise" && i + 1 < argc) {
            options.platform_commands.set_xiaomi_noise_mode = true;
            options.platform_commands.requested_noise_mode = argv[++i];
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
    return !options.show_version && !options.check_updates && !options.json_output && !options.cli_output &&
           (prefer_gui || options.gui_forced);
}

}  // namespace battery_monitor
