#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace battery_monitor {

struct DeviceProfileMatch {
    std::vector<std::string> name_contains;
    std::vector<std::string> device_id_contains;
};

struct DeviceProfileCapability {
    bool enabled = false;
    std::string reader;
    std::string transport;
    std::string strategy;
};

struct DeviceProfile {
    std::string id;
    std::string display_name;
    std::vector<std::string> platforms;
    std::string vendor;
    std::string family;
    std::vector<std::string> device_categories;
    DeviceProfileMatch match;
    DeviceProfileCapability battery;
    DeviceProfileCapability noise_control;
    std::filesystem::path source_path;
};

struct LoadedDeviceProfiles {
    std::filesystem::path directory;
    std::vector<DeviceProfile> profiles;
    std::vector<std::string> warnings;
};

struct DeviceProfileQuery {
    std::string platform;
    std::string primary_name;
    std::string secondary_name;
    std::string device_id;
};

std::filesystem::path ResolveDefaultDeviceProfileDirectory();
LoadedDeviceProfiles LoadDeviceProfilesFromDirectory(const std::filesystem::path& directory);
const LoadedDeviceProfiles& GetCachedDeviceProfiles();
std::vector<const DeviceProfile*> FindMatchingDeviceProfiles(const LoadedDeviceProfiles& loaded_profiles,
                                                            const DeviceProfileQuery& query);
bool AnyMatchingDeviceProfileHasFamily(const LoadedDeviceProfiles& loaded_profiles,
                                       const DeviceProfileQuery& query,
                                       const std::string& family);
bool AnyMatchingDeviceProfileHasCategory(const LoadedDeviceProfiles& loaded_profiles,
                                         const DeviceProfileQuery& query,
                                         const std::string& category);

}  // namespace battery_monitor
