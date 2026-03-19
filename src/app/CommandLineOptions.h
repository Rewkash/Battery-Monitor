#pragma once

#include <string>

namespace battery_monitor {

struct PlatformCommandOptions {
    bool probe_xiaomi_noise = false;
    bool observe_xiaomi_control = false;
    bool observe_zmi_serial = false;
    bool dump_bluetooth_services = false;
    bool dump_ble_gatt = false;
    bool set_xiaomi_noise_mode = false;
    bool send_xiaomi_candidate = false;
    bool set_xiaomi_submode = false;
    std::string device_hint;
    std::string requested_noise_mode;
    std::string requested_submode_family;
    int requested_candidate_id = 0;
    int requested_submode = 0;
    int observe_seconds = 45;

    [[nodiscard]] bool HasAnyCommand() const noexcept;
};

struct CommandLineOptions {
    bool json_output = false;
    bool cli_output = false;
    bool gui_forced = false;
    bool include_offline = false;
    PlatformCommandOptions platform_commands;
};

[[nodiscard]] CommandLineOptions ParseCommandLine(int argc, char** argv);
[[nodiscard]] bool ShouldLaunchGui(const CommandLineOptions& options, bool prefer_gui) noexcept;

}  // namespace battery_monitor
