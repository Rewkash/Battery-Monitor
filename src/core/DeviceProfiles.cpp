#include "core/DeviceProfiles.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

namespace battery_monitor {

namespace {

struct JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue>;

struct JsonValue {
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject>;
    Storage storage = nullptr;
};

class JsonParser {
   public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    JsonValue Parse() {
        SkipWhitespace();
        JsonValue value = ParseValue();
        SkipWhitespace();
        if (!AtEnd()) {
            throw std::runtime_error("unexpected trailing characters");
        }
        return value;
    }

   private:
    JsonValue ParseValue() {
        if (AtEnd()) {
            throw std::runtime_error("unexpected end of input");
        }

        const char ch = input_[position_];
        if (ch == '{') {
            return JsonValue{ParseObject()};
        }
        if (ch == '[') {
            return JsonValue{ParseArray()};
        }
        if (ch == '"') {
            return JsonValue{ParseString()};
        }
        if (ch == 't') {
            ConsumeLiteral("true");
            return JsonValue{true};
        }
        if (ch == 'f') {
            ConsumeLiteral("false");
            return JsonValue{false};
        }
        if (ch == 'n') {
            ConsumeLiteral("null");
            return JsonValue{nullptr};
        }
        if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            return JsonValue{ParseNumber()};
        }

        throw std::runtime_error("unexpected token");
    }

    JsonObject ParseObject() {
        Expect('{');
        SkipWhitespace();

        JsonObject object;
        if (TryConsume('}')) {
            return object;
        }

        while (true) {
            SkipWhitespace();
            const std::string key = ParseString();
            SkipWhitespace();
            Expect(':');
            SkipWhitespace();
            object.emplace(key, ParseValue());
            SkipWhitespace();
            if (TryConsume('}')) {
                break;
            }
            Expect(',');
        }

        return object;
    }

    JsonArray ParseArray() {
        Expect('[');
        SkipWhitespace();

        JsonArray array;
        if (TryConsume(']')) {
            return array;
        }

        while (true) {
            SkipWhitespace();
            array.push_back(ParseValue());
            SkipWhitespace();
            if (TryConsume(']')) {
                break;
            }
            Expect(',');
        }

        return array;
    }

    std::string ParseString() {
        Expect('"');

        std::string result;
        while (!AtEnd()) {
            const char ch = input_[position_++];
            if (ch == '"') {
                return result;
            }

            if (ch != '\\') {
                result.push_back(ch);
                continue;
            }

            if (AtEnd()) {
                throw std::runtime_error("unterminated escape");
            }

            const char escaped = input_[position_++];
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    result.push_back(escaped);
                    break;
                case 'b':
                    result.push_back('\b');
                    break;
                case 'f':
                    result.push_back('\f');
                    break;
                case 'n':
                    result.push_back('\n');
                    break;
                case 'r':
                    result.push_back('\r');
                    break;
                case 't':
                    result.push_back('\t');
                    break;
                case 'u':
                    SkipUnicodeEscape(&result);
                    break;
                default:
                    throw std::runtime_error("unsupported escape sequence");
            }
        }

        throw std::runtime_error("unterminated string");
    }

    double ParseNumber() {
        const std::size_t start = position_;
        if (input_[position_] == '-') {
            ++position_;
        }

        ConsumeDigits();
        if (!AtEnd() && input_[position_] == '.') {
            ++position_;
            ConsumeDigits();
        }

        if (!AtEnd() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (!AtEnd() && (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            ConsumeDigits();
        }

        return std::stod(std::string(input_.substr(start, position_ - start)));
    }

    void ConsumeDigits() {
        if (AtEnd() || std::isdigit(static_cast<unsigned char>(input_[position_])) == 0) {
            throw std::runtime_error("expected digits");
        }

        while (!AtEnd() && std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) {
            ++position_;
        }
    }

    void ConsumeLiteral(std::string_view literal) {
        if (input_.substr(position_, literal.size()) != literal) {
            throw std::runtime_error("unexpected literal");
        }
        position_ += literal.size();
    }

    void SkipWhitespace() {
        while (!AtEnd() && std::isspace(static_cast<unsigned char>(input_[position_])) != 0) {
            ++position_;
        }
    }

    void SkipUnicodeEscape(std::string* output) {
        if (output == nullptr || position_ + 4 > input_.size()) {
            throw std::runtime_error("invalid unicode escape");
        }

        const std::string hex = std::string(input_.substr(position_, 4));
        position_ += 4;
        const unsigned int code_point = static_cast<unsigned int>(std::stoul(hex, nullptr, 16));
        if (code_point <= 0x7FU) {
            output->push_back(static_cast<char>(code_point));
        } else {
            output->push_back('?');
        }
    }

    void Expect(char token) {
        if (AtEnd() || input_[position_] != token) {
            throw std::runtime_error(std::string("expected '") + token + "'");
        }
        ++position_;
    }

    bool TryConsume(char token) {
        if (!AtEnd() && input_[position_] == token) {
            ++position_;
            return true;
        }
        return false;
    }

    bool AtEnd() const { return position_ >= input_.size(); }

    std::string_view input_;
    std::size_t position_ = 0;
};

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

const JsonObject* AsObject(const JsonValue& value) {
    return std::get_if<JsonObject>(&value.storage);
}

const JsonArray* AsArray(const JsonValue& value) {
    return std::get_if<JsonArray>(&value.storage);
}

const std::string* AsString(const JsonValue& value) {
    return std::get_if<std::string>(&value.storage);
}

const bool* AsBool(const JsonValue& value) {
    return std::get_if<bool>(&value.storage);
}

const double* AsNumber(const JsonValue& value) {
    return std::get_if<double>(&value.storage);
}

const JsonValue* FindObjectValue(const JsonObject& object, std::string_view key) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        return nullptr;
    }
    return &it->second;
}

std::optional<std::string> ReadStringField(const JsonObject& object, std::string_view key) {
    const JsonValue* value = FindObjectValue(object, key);
    if (value == nullptr) {
        return std::nullopt;
    }

    if (const auto* text = AsString(*value); text != nullptr) {
        return *text;
    }

    throw std::runtime_error("field '" + std::string(key) + "' must be a string");
}

std::vector<std::string> ReadStringArrayField(const JsonObject& object, std::string_view key) {
    const JsonValue* value = FindObjectValue(object, key);
    if (value == nullptr) {
        return {};
    }

    const auto* array = AsArray(*value);
    if (array == nullptr) {
        throw std::runtime_error("field '" + std::string(key) + "' must be an array");
    }

    std::vector<std::string> result;
    result.reserve(array->size());
    for (const auto& item : *array) {
        const auto* text = AsString(item);
        if (text == nullptr) {
            throw std::runtime_error("field '" + std::string(key) + "' must contain only strings");
        }
        result.push_back(*text);
    }

    return result;
}

bool ReadBooleanField(const JsonObject& object, std::string_view key, bool fallback) {
    const JsonValue* value = FindObjectValue(object, key);
    if (value == nullptr) {
        return fallback;
    }

    if (const auto* flag = AsBool(*value); flag != nullptr) {
        return *flag;
    }

    throw std::runtime_error("field '" + std::string(key) + "' must be a boolean");
}

DeviceProfileCapability ParseCapability(const JsonObject& object, std::string_view key) {
    DeviceProfileCapability capability;

    const JsonValue* value = FindObjectValue(object, key);
    if (value == nullptr) {
        return capability;
    }

    const auto* capability_object = AsObject(*value);
    if (capability_object == nullptr) {
        throw std::runtime_error("field '" + std::string(key) + "' must be an object");
    }

    capability.enabled = ReadBooleanField(*capability_object, "enabled", false);
    capability.reader = ReadStringField(*capability_object, "reader").value_or("");
    capability.transport = ReadStringField(*capability_object, "transport").value_or("");
    capability.strategy = ReadStringField(*capability_object, "strategy").value_or("");
    return capability;
}

DeviceProfileMatch ParseMatch(const JsonObject& object) {
    DeviceProfileMatch match;
    const JsonValue* value = FindObjectValue(object, "match");
    if (value == nullptr) {
        throw std::runtime_error("field 'match' is required");
    }

    const auto* match_object = AsObject(*value);
    if (match_object == nullptr) {
        throw std::runtime_error("field 'match' must be an object");
    }

    match.name_contains = ReadStringArrayField(*match_object, "nameContains");
    match.device_id_contains = ReadStringArrayField(*match_object, "deviceIdContains");
    if (match.name_contains.empty() && match.device_id_contains.empty()) {
        throw std::runtime_error("field 'match' must contain at least one matcher");
    }

    return match;
}

std::vector<std::string> NormalizeStringVector(const std::vector<std::string>& values) {
    std::vector<std::string> normalized;
    normalized.reserve(values.size());
    for (const auto& value : values) {
        if (value.empty()) {
            continue;
        }
        normalized.push_back(ToLowerAscii(value));
    }
    return normalized;
}

DeviceProfile ParseDeviceProfile(const JsonObject& object, const std::filesystem::path& source_path) {
    if (const auto* schema_value = FindObjectValue(object, "schemaVersion"); schema_value != nullptr) {
        const auto* schema_number = AsNumber(*schema_value);
        if (schema_number == nullptr || static_cast<int>(*schema_number) != 1) {
            throw std::runtime_error("unsupported schemaVersion; expected 1");
        }
    }

    DeviceProfile profile;
    profile.id = ReadStringField(object, "id").value_or("");
    if (profile.id.empty()) {
        throw std::runtime_error("field 'id' is required");
    }

    profile.display_name = ReadStringField(object, "displayName").value_or(profile.id);
    profile.platforms = NormalizeStringVector(ReadStringArrayField(object, "platforms"));
    if (profile.platforms.empty()) {
        profile.platforms = {"any"};
    }

    profile.vendor = ToLowerAscii(ReadStringField(object, "vendor").value_or(""));
    profile.family = ToLowerAscii(ReadStringField(object, "family").value_or(""));
    profile.device_categories = NormalizeStringVector(ReadStringArrayField(object, "deviceCategories"));
    profile.match = ParseMatch(object);
    profile.match.name_contains = NormalizeStringVector(profile.match.name_contains);
    profile.match.device_id_contains = NormalizeStringVector(profile.match.device_id_contains);
    profile.battery = ParseCapability(object, "battery");
    profile.noise_control = ParseCapability(object, "noiseControl");
    profile.source_path = source_path;
    return profile;
}

std::optional<std::string> ReadFileUtf8(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

bool ProfileSupportsPlatform(const DeviceProfile& profile, const std::string& platform) {
    const std::string normalized_platform = ToLowerAscii(platform);
    return std::any_of(profile.platforms.begin(), profile.platforms.end(), [&](const std::string& candidate) {
        return candidate == "any" || candidate == normalized_platform;
    });
}

bool ContainsToken(std::string_view haystack, const std::vector<std::string>& tokens) {
    return std::any_of(tokens.begin(), tokens.end(), [&](const std::string& token) {
        return !token.empty() && haystack.find(token) != std::string_view::npos;
    });
}

bool ProfileMatchesQuery(const DeviceProfile& profile, const DeviceProfileQuery& query) {
    if (!ProfileSupportsPlatform(profile, query.platform)) {
        return false;
    }

    const std::string combined_names =
        ToLowerAscii(query.primary_name + " " + query.secondary_name + " " + query.device_id);
    const std::string lowered_device_id = ToLowerAscii(query.device_id);

    const bool name_match =
        profile.match.name_contains.empty() || ContainsToken(combined_names, profile.match.name_contains);
    const bool device_id_match =
        profile.match.device_id_contains.empty() || ContainsToken(lowered_device_id, profile.match.device_id_contains);

    return name_match && device_id_match;
}

std::filesystem::path ResolveProfilesDirectoryFromSearchRoots() {
    const auto current_path = std::filesystem::current_path();
    for (auto probe = current_path; !probe.empty(); probe = probe.parent_path()) {
        const auto candidate = probe / "profiles" / "devices";
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
        if (probe == probe.root_path()) {
            break;
        }
    }

    return current_path / "profiles" / "devices";
}

}  // namespace

std::filesystem::path ResolveDefaultDeviceProfileDirectory() {
    char* env_value = nullptr;
    std::size_t env_length = 0;
    const errno_t env_status = _dupenv_s(&env_value, &env_length, "BATTERY_MONITOR_PROFILE_DIR");
    if (env_status == 0 && env_value != nullptr && *env_value != '\0') {
        const std::filesystem::path directory = env_value;
        free(env_value);
        return directory;
    }
    free(env_value);

    return ResolveProfilesDirectoryFromSearchRoots();
}

LoadedDeviceProfiles LoadDeviceProfilesFromDirectory(const std::filesystem::path& directory) {
    LoadedDeviceProfiles loaded_profiles;
    loaded_profiles.directory = directory;

    std::error_code ec;
    if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_directory(directory, ec)) {
        loaded_profiles.warnings.push_back(
            "Device profile directory was not found: " + directory.string());
        return loaded_profiles;
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, ec)) {
        if (ec) {
            loaded_profiles.warnings.push_back(
                "Failed to iterate device profiles in " + directory.string());
            return loaded_profiles;
        }

        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".json") {
            continue;
        }
        files.push_back(entry.path());
    }

    std::sort(files.begin(), files.end());
    for (const auto& file_path : files) {
        try {
            const auto content = ReadFileUtf8(file_path);
            if (!content.has_value()) {
                loaded_profiles.warnings.push_back(
                    "Failed to read device profile file: " + file_path.string());
                continue;
            }

            JsonParser parser(*content);
            const JsonValue root_value = parser.Parse();
            const auto* root_object = AsObject(root_value);
            if (root_object == nullptr) {
                throw std::runtime_error("root JSON value must be an object");
            }

            loaded_profiles.profiles.push_back(ParseDeviceProfile(*root_object, file_path));
        } catch (const std::exception& error) {
            loaded_profiles.warnings.push_back(
                "Failed to load device profile '" + file_path.string() + "': " + error.what());
        }
    }

    return loaded_profiles;
}

const LoadedDeviceProfiles& GetCachedDeviceProfiles() {
    static const LoadedDeviceProfiles loaded_profiles =
        LoadDeviceProfilesFromDirectory(ResolveDefaultDeviceProfileDirectory());
    return loaded_profiles;
}

std::vector<const DeviceProfile*> FindMatchingDeviceProfiles(const LoadedDeviceProfiles& loaded_profiles,
                                                            const DeviceProfileQuery& query) {
    std::vector<const DeviceProfile*> matches;
    for (const auto& profile : loaded_profiles.profiles) {
        if (ProfileMatchesQuery(profile, query)) {
            matches.push_back(&profile);
        }
    }
    return matches;
}

bool AnyMatchingDeviceProfileHasFamily(const LoadedDeviceProfiles& loaded_profiles,
                                       const DeviceProfileQuery& query,
                                       const std::string& family) {
    const std::string normalized_family = ToLowerAscii(family);
    return std::any_of(loaded_profiles.profiles.begin(), loaded_profiles.profiles.end(),
                       [&](const DeviceProfile& profile) {
                           return profile.family == normalized_family && ProfileMatchesQuery(profile, query);
                       });
}

bool AnyMatchingDeviceProfileHasCategory(const LoadedDeviceProfiles& loaded_profiles,
                                         const DeviceProfileQuery& query,
                                         const std::string& category) {
    const std::string normalized_category = ToLowerAscii(category);
    return std::any_of(loaded_profiles.profiles.begin(), loaded_profiles.profiles.end(),
                       [&](const DeviceProfile& profile) {
                           if (!ProfileMatchesQuery(profile, query)) {
                               return false;
                           }

                           return std::find(profile.device_categories.begin(),
                                            profile.device_categories.end(),
                                            normalized_category) != profile.device_categories.end();
                       });
}

}  // namespace battery_monitor
