#pragma once

#include <utility>
#include <string>
#include <vector>

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
    virtual bool SupportsNoiseSubmodes(const std::string& device_id, NoiseControlMode mode) {
        (void)device_id;
        (void)mode;
        return false;
    }
    virtual std::vector<std::pair<std::string, std::string>> GetNoiseSubmodes(const std::string& device_id,
                                                                               NoiseControlMode mode) {
        (void)device_id;
        (void)mode;
        return {};
    }
    virtual bool SetNoiseSubmode(const std::string& device_id,
                                 NoiseControlMode mode,
                                 const std::string& submode_id) {
        (void)device_id;
        (void)mode;
        (void)submode_id;
        return false;
    }
};

}  // namespace battery_monitor
