#pragma once

#include <string>

namespace battery_monitor {

struct PlatformCommandOptions {
    bool set_xiaomi_noise_mode = false;
    bool set_xiaomi_submode = false;
    std::string device_hint;
    std::string requested_noise_mode;
    std::string requested_submode_family;
    int requested_submode = 0;

    [[nodiscard]] bool HasAnyCommand() const noexcept;
};

struct CommandLineOptions {
    bool show_version = false;
    bool check_updates = false;
    bool json_output = false;
    bool cli_output = false;
    bool gui_forced = false;
    bool include_offline = false;
    PlatformCommandOptions platform_commands;
};

[[nodiscard]] CommandLineOptions ParseCommandLine(int argc, char** argv);
[[nodiscard]] bool ShouldLaunchGui(const CommandLineOptions& options, bool prefer_gui) noexcept;

}  // namespace battery_monitor
