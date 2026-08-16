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

// Result of the most-specific profile selection. `profile` is null when no
// profile matches the query. `notes` records diagnostics such as
// equal-specificity conflicts between candidate profiles.
struct DeviceProfileSelection {
    const DeviceProfile* profile = nullptr;
    std::vector<std::string> notes;
};

// Selects the single most specific profile matching the query. Specificity is
// determined by the matched deviceIdContains tokens first (total matched
// length), then by the matched nameContains tokens (total matched length).
// Ties are broken by the lexicographically smallest profile id.
DeviceProfileSelection SelectDeviceProfile(const LoadedDeviceProfiles& loaded_profiles,
                                           const DeviceProfileQuery& query);

// Kept for compatibility; returns at most one entry (the most specific match).
std::vector<const DeviceProfile*> FindMatchingDeviceProfiles(const LoadedDeviceProfiles& loaded_profiles,
                                                            const DeviceProfileQuery& query);
bool AnyMatchingDeviceProfileHasFamily(const LoadedDeviceProfiles& loaded_profiles,
                                       const DeviceProfileQuery& query,
                                       const std::string& family);
bool AnyMatchingDeviceProfileHasCategory(const LoadedDeviceProfiles& loaded_profiles,
                                         const DeviceProfileQuery& query,
                                         const std::string& category);

}  // namespace battery_monitor
