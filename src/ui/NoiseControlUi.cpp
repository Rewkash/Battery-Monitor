#include "ui/NoiseControlUi.h"

#include "core/NoiseControlVocabulary.h"

namespace battery_monitor {

namespace {

QString ToQString(const std::string& value) {
    return QString::fromUtf8(value.c_str());
}

std::string ToUtf8(const QString& value) {
    const QByteArray utf8 = value.toUtf8();
    return std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

}  // namespace

QString NormalizeNoiseToken(const QString& value) {
    return ToQString(NormalizeNoiseControlToken(ToUtf8(value)));
}

QString NoiseModeLabel(const QString& mode, NoiseControlModeLabelStyle style) {
    const QString normalized = NormalizeNoiseToken(mode);
    if (normalized == QStringLiteral("off")) {
        return style == NoiseControlModeLabelStyle::Compact
                   ? QString::fromUtf8(u8"Выкл")
                   : QString::fromUtf8(u8"Выключено");
    }
    if (normalized == QStringLiteral("anc")) {
        return QString::fromUtf8(u8"Шумоподавление");
    }
    if (normalized == QStringLiteral("transparency")) {
        return QString::fromUtf8(u8"Прозрачность");
    }

    return mode.trimmed();
}

QString NoiseSubmodeLabel(const QString& submode) {
    const QString normalized = NormalizeNoiseToken(submode);
    if (normalized == QStringLiteral("balanced")) {
        return QString::fromUtf8(u8"Баланс");
    }
    if (normalized == QStringLiteral("weak")) {
        return QString::fromUtf8(u8"Слабое");
    }
    if (normalized == QStringLiteral("deep")) {
        return QString::fromUtf8(u8"Глубокое");
    }
    if (normalized == QStringLiteral("adaptive")) {
        return QString::fromUtf8(u8"Адаптивное");
    }
    if (normalized == QStringLiteral("standard")) {
        return QString::fromUtf8(u8"Обычная прозрачность");
    }
    if (normalized == QStringLiteral("voice")) {
        return QString::fromUtf8(u8"Усиление голоса");
    }

    return submode.trimmed();
}

bool NoiseModeNeedsSubmode(const QString& mode) {
    const auto parsed = ParseNoiseControlModeToken(ToUtf8(mode));
    return parsed.has_value() && NoiseControlModeSupportsSubmodes(*parsed);
}

QString DefaultNoiseSubmodeToken(const QString& mode) {
    const auto parsed = ParseNoiseControlModeToken(ToUtf8(mode));
    if (!parsed.has_value()) {
        return {};
    }

    return ToQString(GetDefaultNoiseControlSubmodeToken(*parsed));
}

QVector<QPair<QString, QString>> NoiseSubmodeItems(const QString& mode) {
    QVector<QPair<QString, QString>> items;
    const auto parsed = ParseNoiseControlModeToken(ToUtf8(mode));
    if (!parsed.has_value()) {
        return items;
    }

    const auto submodes = GetNoiseControlSubmodes(*parsed);
    items.reserve(static_cast<qsizetype>(submodes.size()));
    for (const auto& [id, label] : submodes) {
        items.push_back(qMakePair(ToQString(id), ToQString(label)));
    }
    return items;
}

}  // namespace battery_monitor
