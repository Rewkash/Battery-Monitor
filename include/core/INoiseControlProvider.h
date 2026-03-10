#pragma once

#include <string>

namespace battery_monitor {

enum class NoiseControlMode {
    Off,
    Anc,
    Transparency,
};

class INoiseControlProvider {
   public:
    virtual ~INoiseControlProvider() = default;

    virtual bool SupportsNoiseControl(const std::string& device_id) = 0;
    virtual bool SetNoiseControlMode(const std::string& device_id, NoiseControlMode mode) = 0;
};

}  // namespace battery_monitor
