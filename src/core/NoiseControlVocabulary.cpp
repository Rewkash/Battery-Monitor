#include "core/NoiseControlVocabulary.h"

#include <algorithm>
#include <cctype>

namespace battery_monitor {

namespace {

std::string TrimAscii(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }

    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return std::string(value.substr(begin, end - begin));
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

}  // namespace

std::string NormalizeNoiseControlToken(std::string_view value) {
    const std::string normalized = ToLowerAscii(TrimAscii(value));
    if (normalized == "disable" || normalized == "disabled") {
        return "off";
    }
    if (normalized == "noise" || normalized == "noise-canceling" || normalized == "noise cancelling" ||
        normalized == "noise-cancelling") {
        return "anc";
    }
    if (normalized == "transparent") {
        return "transparency";
    }
    if (normalized == "balance") {
        return "balanced";
    }
    if (normalized == "normal") {
        return "standard";
    }

    return normalized;
}

std::optional<NoiseControlMode> ParseNoiseControlModeToken(std::string_view value) {
    const std::string normalized = NormalizeNoiseControlToken(value);
    if (normalized == "off") {
        return NoiseControlMode::Off;
    }
    if (normalized == "anc") {
        return NoiseControlMode::Anc;
    }
    if (normalized == "transparency") {
        return NoiseControlMode::Transparency;
    }

    return std::nullopt;
}

std::string NoiseControlModeToken(NoiseControlMode mode) {
    switch (mode) {
        case NoiseControlMode::Off:
            return "off";
        case NoiseControlMode::Anc:
            return "anc";
        case NoiseControlMode::Transparency:
            return "transparency";
    }

    return {};
}

bool NoiseControlModeSupportsSubmodes(NoiseControlMode mode) noexcept {
    return mode == NoiseControlMode::Anc || mode == NoiseControlMode::Transparency;
}

std::vector<std::pair<std::string, std::string>> GetNoiseControlSubmodes(NoiseControlMode mode) {
    switch (mode) {
        case NoiseControlMode::Anc:
            return {
                {"balanced", "Баланс"},
                {"weak", "Слабое"},
                {"deep", "Глубокое"},
                {"adaptive", "Адаптивное"},
            };
        case NoiseControlMode::Transparency:
            return {
                {"standard", "Обычная прозрачность"},
                {"voice", "Усиление голоса"},
            };
        case NoiseControlMode::Off:
            return {};
    }

    return {};
}

std::string GetDefaultNoiseControlSubmodeToken(NoiseControlMode mode) {
    switch (mode) {
        case NoiseControlMode::Anc:
            return "balanced";
        case NoiseControlMode::Transparency:
            return "standard";
        case NoiseControlMode::Off:
            return {};
    }

    return {};
}

}  // namespace battery_monitor
