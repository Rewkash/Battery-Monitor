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

constexpr std::size_t kMaxNestingDepth = 64;
constexpr std::size_t kMaxJsonFileSize = 16U * 1024U * 1024U;

void AppendUtf8(std::string* output, unsigned int code_point) {
    if (code_point <= 0x7FU) {
        output->push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FFU) {
        output->push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        output->push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point <= 0xFFFFU) {
        output->push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        output->push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output->push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        output->push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
        output->push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
        output->push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output->push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

class DepthGuard {
   public:
    explicit DepthGuard(std::size_t* depth) : depth_(depth) {
        if (*depth_ >= kMaxNestingDepth) {
            throw std::runtime_error("maximum JSON nesting depth exceeded");
        }
        ++(*depth_);
    }
    ~DepthGuard() { --(*depth_); }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;

   private:
    std::size_t* depth_;
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
        DepthGuard guard(&depth_);
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
        DepthGuard guard(&depth_);
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
                if (static_cast<unsigned char>(ch) >= 0x80U) {
                    AppendValidatedUtf8Sequence(ch, &result);
                    continue;
                }
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
                    DecodeUnicodeEscape(&result);
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

    unsigned int ParseHex4() {
        if (position_ + 4 > input_.size()) {
            throw std::runtime_error("truncated \\u escape");
        }

        unsigned int value = 0;
        for (int digit_index = 0; digit_index < 4; ++digit_index) {
            const char digit = input_[position_++];
            value <<= 4U;
            if (digit >= '0' && digit <= '9') {
                value |= static_cast<unsigned int>(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
                value |= static_cast<unsigned int>(digit - 'a' + 10);
            } else if (digit >= 'A' && digit <= 'F') {
                value |= static_cast<unsigned int>(digit - 'A' + 10);
            } else {
                throw std::runtime_error("invalid hex digit in \\u escape");
            }
        }
        return value;
    }

    void DecodeUnicodeEscape(std::string* output) {
        if (output == nullptr) {
            throw std::runtime_error("invalid unicode escape");
        }

        unsigned int code_point = ParseHex4();
        if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
            // High surrogate: must be followed by a matching low surrogate.
            if (position_ + 1 >= input_.size() || input_[position_] != '\\' ||
                input_[position_ + 1] != 'u') {
                throw std::runtime_error("unpaired high surrogate in \\u escape");
            }
            position_ += 2;
            const unsigned int low_surrogate = ParseHex4();
            if (low_surrogate < 0xDC00U || low_surrogate > 0xDFFFU) {
                throw std::runtime_error("invalid low surrogate in \\u escape");
            }
            code_point = 0x10000U + ((code_point - 0xD800U) << 10U) + (low_surrogate - 0xDC00U);
        } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
            throw std::runtime_error("unpaired low surrogate in \\u escape");
        }

        AppendUtf8(output, code_point);
    }

    void AppendValidatedUtf8Sequence(char lead_byte, std::string* output) {
        const auto lead = static_cast<unsigned char>(lead_byte);
        unsigned int length = 0;
        unsigned int code_point = 0;
        if ((lead & 0xE0U) == 0xC0U) {
            length = 2;
            code_point = lead & 0x1FU;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
            code_point = lead & 0x0FU;
        } else if ((lead & 0xF8U) == 0xF0U) {
            length = 4;
            code_point = lead & 0x07U;
        } else {
            throw std::runtime_error("invalid UTF-8 lead byte in string");
        }

        if (position_ + (length - 1) > input_.size()) {
            throw std::runtime_error("truncated UTF-8 sequence in string");
        }

        output->push_back(lead_byte);
        for (unsigned int byte_index = 1; byte_index < length; ++byte_index) {
            const auto continuation = static_cast<unsigned char>(input_[position_]);
            if ((continuation & 0xC0U) != 0x80U) {
                throw std::runtime_error("invalid UTF-8 continuation byte in string");
            }
            code_point = (code_point << 6U) | (continuation & 0x3FU);
            output->push_back(input_[position_++]);
        }

        const bool overlong = (length == 2 && code_point < 0x80U) ||
                              (length == 3 && code_point < 0x800U) ||
                              (length == 4 && code_point < 0x10000U);
        const bool surrogate = code_point >= 0xD800U && code_point <= 0xDFFFU;
        if (overlong || surrogate || code_point > 0x10FFFFU) {
            throw std::runtime_error("invalid UTF-8 code point in string");
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
    std::size_t depth_ = 0;
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

bool IsBlank(const std::string& value) {
    return std::all_of(value.begin(), value.end(),
                       [](unsigned char ch) { return std::isspace(ch) != 0; });
}

const std::vector<std::string>& AllowedPlatforms() {
    static const std::vector<std::string> values = {"any", "windows", "linux", "macos", "android", "ios"};
    return values;
}

const std::vector<std::string>& AllowedDeviceCategories() {
    static const std::vector<std::string> values = {
        "tws", "headphone", "headset", "earbuds", "speaker", "phone", "tablet",
        "laptop", "watch", "mouse", "keyboard", "controller", "pen", "other"};
    return values;
}

const std::vector<std::string>& AllowedTransports() {
    static const std::vector<std::string> values = {"rfcomm", "ble", "hid", "usb", "serial", "any"};
    return values;
}

void ValidateEnumValue(std::string_view field_name,
                       const std::string& value,
                       const std::vector<std::string>& allowed) {
    if (std::find(allowed.begin(), allowed.end(), value) != allowed.end()) {
        return;
    }

    std::string message = "field '" + std::string(field_name) + "' has unsupported value '" + value +
                          "' (allowed:";
    for (const auto& candidate : allowed) {
        message += " " + candidate + ",";
    }
    if (!allowed.empty()) {
        message.pop_back();
    }
    message += ")";
    throw std::runtime_error(message);
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

    if (capability.enabled) {
        if (capability.reader.empty()) {
            throw std::runtime_error("field '" + std::string(key) +
                                     ".reader' must be a non-empty string when enabled");
        }
        if (!capability.transport.empty()) {
            ValidateEnumValue(std::string(key) + ".transport", ToLowerAscii(capability.transport),
                              AllowedTransports());
        }
    }

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

    const auto reject_blank_matchers = [](const std::vector<std::string>& tokens,
                                          const char* field_name) {
        for (const auto& token : tokens) {
            if (IsBlank(token)) {
                throw std::runtime_error(std::string("field 'match.") + field_name +
                                         "' must not contain blank entries");
            }
        }
    };
    reject_blank_matchers(match.name_contains, "nameContains");
    reject_blank_matchers(match.device_id_contains, "deviceIdContains");

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
    const auto* schema_value = FindObjectValue(object, "schemaVersion");
    if (schema_value == nullptr) {
        throw std::runtime_error("field 'schemaVersion' is required and must be the number 1");
    }
    const auto* schema_number = AsNumber(*schema_value);
    if (schema_number == nullptr || static_cast<int>(*schema_number) != 1) {
        throw std::runtime_error("unsupported schemaVersion; expected 1");
    }

    DeviceProfile profile;
    profile.id = ReadStringField(object, "id").value_or("");
    if (profile.id.empty() || IsBlank(profile.id)) {
        throw std::runtime_error("field 'id' is required and must be a non-blank string");
    }

    profile.display_name = ReadStringField(object, "displayName").value_or(profile.id);
    profile.platforms = NormalizeStringVector(ReadStringArrayField(object, "platforms"));
    if (profile.platforms.empty()) {
        profile.platforms = {"any"};
    }
    for (const auto& platform : profile.platforms) {
        ValidateEnumValue("platforms", platform, AllowedPlatforms());
    }

    profile.vendor = ToLowerAscii(ReadStringField(object, "vendor").value_or(""));
    profile.family = ToLowerAscii(ReadStringField(object, "family").value_or(""));
    profile.device_categories = NormalizeStringVector(ReadStringArrayField(object, "deviceCategories"));
    for (const auto& category : profile.device_categories) {
        ValidateEnumValue("deviceCategories", category, AllowedDeviceCategories());
    }
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

bool ProfileMatchesQuery(const DeviceProfile& profile,
                         const DeviceProfileQuery& query,
                         const std::string& combined_names,
                         const std::string& lowered_device_id) {
    if (!ProfileSupportsPlatform(profile, query.platform)) {
        return false;
    }

    const bool name_match =
        profile.match.name_contains.empty() || ContainsToken(combined_names, profile.match.name_contains);
    const bool device_id_match =
        profile.match.device_id_contains.empty() ||
        ContainsToken(lowered_device_id, profile.match.device_id_contains);

    return name_match && device_id_match;
}

struct MatchSpecificity {
    std::size_t device_id_chars = 0;
    std::size_t name_chars = 0;

    bool operator<(const MatchSpecificity& other) const {
        if (device_id_chars != other.device_id_chars) {
            return device_id_chars < other.device_id_chars;
        }
        return name_chars < other.name_chars;
    }
    bool operator==(const MatchSpecificity& other) const {
        return device_id_chars == other.device_id_chars && name_chars == other.name_chars;
    }
};

std::size_t MatchedTokenChars(std::string_view haystack, const std::vector<std::string>& tokens) {
    std::size_t total = 0;
    for (const auto& token : tokens) {
        if (!token.empty() && haystack.find(token) != std::string_view::npos) {
            total += token.size();
        }
    }
    return total;
}

MatchSpecificity ComputeSpecificity(const DeviceProfile& profile,
                                    const std::string& combined_names,
                                    const std::string& lowered_device_id) {
    MatchSpecificity specificity;
    specificity.device_id_chars = MatchedTokenChars(lowered_device_id, profile.match.device_id_contains);
    specificity.name_chars = MatchedTokenChars(combined_names, profile.match.name_contains);
    return specificity;
}

DeviceProfileSelection SelectBestDeviceProfile(const LoadedDeviceProfiles& loaded_profiles,
                                               const DeviceProfileQuery& query) {
    const std::string combined_names =
        ToLowerAscii(query.primary_name + " " + query.secondary_name + " " + query.device_id);
    const std::string lowered_device_id = ToLowerAscii(query.device_id);

    struct Candidate {
        const DeviceProfile* profile;
        MatchSpecificity specificity;
    };
    std::vector<Candidate> candidates;

    for (const auto& profile : loaded_profiles.profiles) {
        if (ProfileMatchesQuery(profile, query, combined_names, lowered_device_id)) {
            candidates.push_back({&profile, ComputeSpecificity(profile, combined_names, lowered_device_id)});
        }
    }

    if (candidates.empty()) {
        return DeviceProfileSelection{};
    }

    MatchSpecificity best_specificity = candidates.front().specificity;
    for (const auto& candidate : candidates) {
        if (best_specificity < candidate.specificity) {
            best_specificity = candidate.specificity;
        }
    }

    const DeviceProfile* best = nullptr;
    for (const auto& candidate : candidates) {
        if (!(candidate.specificity == best_specificity)) {
            continue;
        }
        if (best == nullptr || candidate.profile->id < best->id) {
            best = candidate.profile;
        }
    }

    DeviceProfileSelection selection;
    selection.profile = best;
    for (const auto& candidate : candidates) {
        if (candidate.specificity == best_specificity && candidate.profile != best) {
            selection.notes.push_back(
                "equal-specificity conflict: profile '" + best->id + "' wins over '" +
                candidate.profile->id + "' by lexicographic id tie-breaker (both match with " +
                "deviceIdChars=" + std::to_string(best_specificity.device_id_chars) + ", nameChars=" +
                std::to_string(best_specificity.name_chars) + ")");
        }
    }
    return selection;
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
    std::map<std::string, std::filesystem::path> loaded_ids;
    for (const auto& file_path : files) {
        try {
            const auto content = ReadFileUtf8(file_path);
            if (!content.has_value()) {
                loaded_profiles.warnings.push_back(
                    "Failed to read device profile file: " + file_path.string());
                continue;
            }

            if (content->size() > kMaxJsonFileSize) {
                throw std::runtime_error("profile file exceeds the maximum supported size");
            }

            JsonParser parser(*content);
            const JsonValue root_value = parser.Parse();
            const auto* root_object = AsObject(root_value);
            if (root_object == nullptr) {
                throw std::runtime_error("root JSON value must be an object");
            }

            DeviceProfile profile = ParseDeviceProfile(*root_object, file_path);
            const auto [existing, inserted] = loaded_ids.emplace(profile.id, file_path);
            if (!inserted) {
                throw std::runtime_error("duplicate profile id '" + profile.id + "'; already defined in '" +
                                         existing->second.string() + "'");
            }
            loaded_profiles.profiles.push_back(std::move(profile));
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

DeviceProfileSelection SelectDeviceProfile(const LoadedDeviceProfiles& loaded_profiles,
                                           const DeviceProfileQuery& query) {
    return SelectBestDeviceProfile(loaded_profiles, query);
}

std::vector<const DeviceProfile*> FindMatchingDeviceProfiles(const LoadedDeviceProfiles& loaded_profiles,
                                                            const DeviceProfileQuery& query) {
    std::vector<const DeviceProfile*> matches;
    const DeviceProfileSelection selection = SelectBestDeviceProfile(loaded_profiles, query);
    if (selection.profile != nullptr) {
        matches.push_back(selection.profile);
    }
    return matches;
}

bool AnyMatchingDeviceProfileHasFamily(const LoadedDeviceProfiles& loaded_profiles,
                                       const DeviceProfileQuery& query,
                                       const std::string& family) {
    const std::string normalized_family = ToLowerAscii(family);
    const DeviceProfileSelection selection = SelectBestDeviceProfile(loaded_profiles, query);
    return selection.profile != nullptr && selection.profile->family == normalized_family;
}

bool AnyMatchingDeviceProfileHasCategory(const LoadedDeviceProfiles& loaded_profiles,
                                         const DeviceProfileQuery& query,
                                         const std::string& category) {
    const std::string normalized_category = ToLowerAscii(category);
    const DeviceProfileSelection selection = SelectBestDeviceProfile(loaded_profiles, query);
    if (selection.profile == nullptr) {
        return false;
    }

    return std::find(selection.profile->device_categories.begin(),
                     selection.profile->device_categories.end(),
                     normalized_category) != selection.profile->device_categories.end();
}

}  // namespace battery_monitor
