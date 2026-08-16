#include "app/CommandLineOptions.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <string>

#include "core/NoiseControlVocabulary.h"

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

bool TryParseSubmodeIndex(const std::string& token, int& value) {
    const char* begin = token.c_str();
    const char* end = begin + token.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

void SetError(CommandLineOptions& options, std::string message) {
    if (!options.has_error) {
        options.has_error = true;
        options.error_message = std::move(message);
    }
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
            options.cli_output = true;
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
        if (arg == "--xiaomi-set-noise") {
            if (i + 1 >= argc || std::string(argv[i + 1]).empty() || argv[i + 1][0] == '-') {
                SetError(options, "option '--xiaomi-set-noise' requires a noise mode value");
                continue;
            }
            options.platform_commands.set_xiaomi_noise_mode = true;
            options.platform_commands.requested_noise_mode = argv[++i];
            if (!ParseNoiseControlModeToken(options.platform_commands.requested_noise_mode).has_value()) {
                SetError(options, "unknown noise mode '" + options.platform_commands.requested_noise_mode +
                                      "' for --xiaomi-set-noise");
            }
            TryConsumeOptionalValue(argc, argv, i, options.platform_commands.device_hint);
            continue;
        }
        if (arg == "--xiaomi-set-submode") {
            if (i + 2 >= argc) {
                SetError(options, "option '--xiaomi-set-submode' requires <family> and <submode> values");
                continue;
            }
            options.platform_commands.requested_submode_family = argv[i + 1];
            if (options.platform_commands.requested_submode_family.empty() ||
                options.platform_commands.requested_submode_family[0] == '-') {
                SetError(options, "option '--xiaomi-set-submode' requires a non-empty <family> value");
                continue;
            }
            if (!TryParseSubmodeIndex(argv[i + 2], options.platform_commands.requested_submode)) {
                SetError(options, std::string("invalid submode index '") + argv[i + 2] +
                                      "' for --xiaomi-set-submode (expected an integer)");
                continue;
            }
            i += 2;
            options.platform_commands.set_xiaomi_submode = true;
            TryConsumeOptionalValue(argc, argv, i, options.platform_commands.device_hint);
            continue;
        }
        SetError(options, "unknown option '" + arg + "'");
    }

    if (options.gui_forced && (options.cli_output || options.json_output || options.check_updates)) {
        SetError(options, "option '--gui' cannot be combined with --cli, --json or --check-updates");
    }

    return options;
}

bool ShouldLaunchGui(const CommandLineOptions& options, bool prefer_gui) noexcept {
    return !options.show_version && !options.check_updates && !options.json_output && !options.cli_output &&
           (prefer_gui || options.gui_forced);
}

std::string CommandLineUsageText() {
    return "Usage: battery-monitor [OPTIONS]\n"
           "Options:\n"
           "  --version                    Print version and exit\n"
           "  --cli                        Force command-line output\n"
           "  --gui                        Force the graphical application\n"
           "  --json                       Print machine-readable JSON (implies --cli)\n"
           "  --all, --include-offline     Include offline/cached devices\n"
           "  --check-updates              Check for application updates and exit\n"
           "  --xiaomi-set-noise <mode> [device]\n"
           "                               Set Xiaomi noise control mode (e.g. off, transparency, noise)\n"
           "  --xiaomi-set-submode <family> <index> [device]\n"
           "                               Set Xiaomi noise control submode by family and index\n";
}

}  // namespace battery_monitor
