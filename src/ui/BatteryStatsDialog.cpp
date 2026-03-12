#include "ui/BatteryStatsDialog.h"

#include "ui/BatteryRuntimeEstimator.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include <QDateTime>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QLocale>
#include <QPushButton>
#include <QStyle>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

namespace battery_monitor {

namespace {

QString DialogTitle(const BatteryHistoryData& history) {
    if (!history.device_name.trimmed().isEmpty()) {
        return history.device_name;
    }
    if (!history.device_id.trimmed().isEmpty()) {
        return history.device_id;
    }
    return QString::fromUtf8(u8"Устройство");
}

QString ModeLabel(const QString& mode) {
    const QString normalized = mode.trimmed().toLower();
    if (normalized == QStringLiteral("off")) {
        return QString::fromUtf8(u8"Выключено");
    }
    if (normalized == QStringLiteral("anc")) {
        return QString::fromUtf8(u8"Шумоподавление");
    }
    if (normalized == QStringLiteral("transparency")) {
        return QString::fromUtf8(u8"Прозрачность");
    }
    return normalized;
}

QString SubmodeLabel(const QString& submode) {
    const QString normalized = submode.trimmed().toLower();
    if (normalized == QStringLiteral("balance")) {
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
    if (normalized == QStringLiteral("normal")) {
        return QString::fromUtf8(u8"Обычная прозрачность");
    }
    if (normalized == QStringLiteral("voice")) {
        return QString::fromUtf8(u8"Усиление голоса");
    }
    return normalized;
}

QString ComponentLabel(const QString& component_key) {
    if (component_key == QStringLiteral("left")) {
        return QString::fromUtf8(u8"Левый");
    }
    if (component_key == QStringLiteral("right")) {
        return QString::fromUtf8(u8"Правый");
    }
    return component_key;
}

QString DisplayComponentLabel(const QString& component_key) {
    if (component_key == QStringLiteral("main")) {
        return QString::fromUtf8(u8"Устройство");
    }
    if (component_key == QStringLiteral("case")) {
        return QString::fromUtf8(u8"Кейс");
    }
    return ComponentLabel(component_key);
}

QList<QString> PresentComponents(const BatteryHistoryData& history) {
    QList<QString> components;
    for (const auto& sample : history.samples) {
        for (auto it = sample.component_levels.cbegin(); it != sample.component_levels.cend(); ++it) {
            if (!components.contains(it.key())) {
                components.push_back(it.key());
            }
        }
    }
    return components;
}

QList<QString> DisplayComponents(const BatteryHistoryData& history) {
    const QList<QString> present = PresentComponents(history);
    QList<QString> ordered;

    if (present.contains(QStringLiteral("left"))) {
        ordered.push_back(QStringLiteral("left"));
    }
    if (present.contains(QStringLiteral("right"))) {
        ordered.push_back(QStringLiteral("right"));
    }
    if (!ordered.isEmpty()) {
        return ordered;
    }

    if (present.contains(QStringLiteral("main"))) {
        ordered.push_back(QStringLiteral("main"));
    }
    if (present.contains(QStringLiteral("case"))) {
        ordered.push_back(QStringLiteral("case"));
    }
    if (!ordered.isEmpty()) {
        return ordered;
    }

    return present;
}

bool HasModeData(const BatteryHistoryData& history) {
    return std::any_of(history.samples.cbegin(), history.samples.cend(), [](const BatteryHistorySample& sample) {
        return !sample.device_mode.trimmed().isEmpty();
    });
}

bool HasStereoEarbudsLayout(const QList<QString>& components) {
    return components.contains(QStringLiteral("left")) && components.contains(QStringLiteral("right"));
}

QString BuildCountdownStateKey(const QString& component_key,
                               const std::optional<int>& level,
                               const QString& mode,
                               const QString& submode) {
    return component_key + QChar('|') +
           (level.has_value() ? QString::number(*level) : QStringLiteral("na")) + QChar('|') +
           mode.trimmed().toLower() + QChar('|') + submode.trimmed().toLower();
}

QString FormatCountdownDuration(qint64 duration_ms) {
    const qint64 total_seconds = std::max<qint64>(0, duration_ms / 1000LL);
    const qint64 hours = total_seconds / 3600LL;
    const qint64 minutes = (total_seconds % 3600LL) / 60LL;
    const qint64 seconds = total_seconds % 60LL;

    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours, 2, 10, QLatin1Char('0'))
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }

    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

QString BadgeText(BatteryRuntimeConfidence confidence, bool has_forecast) {
    if (!has_forecast) {
        return QString::fromUtf8(u8"Недостаточно данных");
    }

    switch (confidence) {
        case BatteryRuntimeConfidence::High:
            return QString::fromUtf8(u8"Высокая");
        case BatteryRuntimeConfidence::Medium:
            return QString::fromUtf8(u8"Предварительно");
        case BatteryRuntimeConfidence::Low:
        default:
            return QString::fromUtf8(u8"Недостаточно данных");
    }
}

QString BadgeState(BatteryRuntimeConfidence confidence, bool has_forecast) {
    if (!has_forecast) {
        return QStringLiteral("low");
    }

    switch (confidence) {
        case BatteryRuntimeConfidence::High:
            return QStringLiteral("high");
        case BatteryRuntimeConfidence::Medium:
            return QStringLiteral("medium");
        case BatteryRuntimeConfidence::Low:
        default:
            return QStringLiteral("low");
    }
}

std::optional<int> SampleLevel(const BatteryHistorySample& sample, const QString& component_key) {
    const auto it = sample.component_levels.find(component_key);
    if (it == sample.component_levels.end()) {
        return std::nullopt;
    }
    return it.value();
}

bool IsVisibleBatteryLevel(const std::optional<int>& level) {
    return level.has_value() && *level > 0;
}

struct ImbalanceSummary {
    int shared_samples = 0;
    double average_diff = 0.0;
    int max_abs_gap = 0;
    std::optional<int> latest_diff;
};

ImbalanceSummary ComputeImbalanceSummary(const BatteryHistoryData& history) {
    ImbalanceSummary summary;
    double total_diff = 0.0;

    for (const auto& sample : history.samples) {
        const auto left = SampleLevel(sample, QStringLiteral("left"));
        const auto right = SampleLevel(sample, QStringLiteral("right"));
        if (!IsVisibleBatteryLevel(left) || !IsVisibleBatteryLevel(right)) {
            continue;
        }

        const int diff = *left - *right;
        total_diff += static_cast<double>(diff);
        summary.max_abs_gap = std::max(summary.max_abs_gap, std::abs(diff));
        ++summary.shared_samples;
    }

    if (summary.shared_samples > 0) {
        summary.average_diff = total_diff / static_cast<double>(summary.shared_samples);
    }

    if (!history.samples.isEmpty()) {
        const auto& latest = history.samples.back();
        const auto left = SampleLevel(latest, QStringLiteral("left"));
        const auto right = SampleLevel(latest, QStringLiteral("right"));
        if (IsVisibleBatteryLevel(left) && IsVisibleBatteryLevel(right)) {
            summary.latest_diff = *left - *right;
        }
    }

    return summary;
}

QString HistoryMeta(const BatteryHistoryData& history) {
    if (history.samples.isEmpty()) {
        return QString::fromUtf8(u8"История ещё не накопилась.");
    }

    const auto start_time = QDateTime::fromMSecsSinceEpoch(history.samples.front().timestamp_ms);
    const auto end_time = QDateTime::fromMSecsSinceEpoch(history.samples.back().timestamp_ms);

    return QString::fromUtf8(u8"История: %1 — %2 · Точек: %3")
        .arg(QLocale::system().toString(start_time, QStringLiteral("dd MMM HH:mm")))
        .arg(QLocale::system().toString(end_time, QStringLiteral("dd MMM HH:mm")))
        .arg(history.samples.size());
}

QString CurrentModeText(const BatteryHistoryData& history) {
    if (history.samples.isEmpty()) {
        return QString::fromUtf8(u8"Сейчас: режим не определён");
    }

    const auto& latest = history.samples.back();
    const QString mode = ModeLabel(latest.device_mode);
    if (mode.isEmpty()) {
        return QString::fromUtf8(u8"Сейчас: режим не определён");
    }

    QString result = QString::fromUtf8(u8"Сейчас: режим «%1»").arg(mode);
    const QString submode = SubmodeLabel(latest.device_submode);
    if (!submode.isEmpty()) {
        result += QString::fromUtf8(u8", «%1»").arg(submode);
    }
    return result;
}

QString ActualModeToken(const BatteryHistoryData& history) {
    if (history.samples.isEmpty()) {
        return QString();
    }
    return history.samples.back().device_mode.trimmed().toLower();
}

QString ActualSubmodeToken(const BatteryHistoryData& history) {
    if (history.samples.isEmpty()) {
        return QString();
    }
    return history.samples.back().device_submode.trimmed().toLower();
}

QString ScenarioText(const QString& mode_token, const QString& submode_token, bool is_current) {
    if (is_current) {
        return QString::fromUtf8(u8"Сценарий прогноза: текущий режим");
    }

    QString text = QString::fromUtf8(u8"Сценарий прогноза: %1").arg(ModeLabel(mode_token));
    const QString submode = SubmodeLabel(submode_token);
    if (!submode.isEmpty()) {
        text += QString::fromUtf8(u8" · %1").arg(submode);
    }
    return text;
}

bool ScenarioNeedsSubmode(const QString& mode_token) {
    return mode_token == QStringLiteral("anc") || mode_token == QStringLiteral("transparency");
}

QString DefaultSubmodeForMode(const QString& mode_token) {
    if (mode_token == QStringLiteral("anc")) {
        return QStringLiteral("balanced");
    }
    if (mode_token == QStringLiteral("transparency")) {
        return QStringLiteral("standard");
    }
    return QString();
}

BatteryHistoryData BuildScenarioHistory(const BatteryHistoryData& history,
                                        const QString& selected_mode,
                                        const QString& selected_submode) {
    if (history.samples.isEmpty() || selected_mode == QStringLiteral("current")) {
        return history;
    }

    BatteryHistoryData scenario = history;
    auto& latest = scenario.samples.back();
    latest.device_mode = selected_mode;
    latest.device_submode = (selected_mode == QStringLiteral("off")) ? QString() : selected_submode;
    return scenario;
}

struct EarCardData {
    QString percent_text;
    QString eta_text;
    QString confidence_text;
    QString confidence_state;
    QString note_text;
    std::optional<qint64> remaining_ms;
};

EarCardData BuildEarCardData(const BatteryHistoryData& history,
                             const BatteryRuntimeForecast& forecast,
                             const QString& component_key) {
    EarCardData data;
    const auto& latest = history.samples.back();
    const auto level = SampleLevel(latest, component_key);
    const bool has_visible_level = IsVisibleBatteryLevel(level);

    if (has_visible_level) {
        data.percent_text = QString::fromUtf8(u8"%1%").arg(*level);
    } else if (level.has_value()) {
        data.percent_text = QString::fromUtf8(u8"Неактивен");
    } else {
        data.percent_text = QString::fromUtf8(u8"Заряд неизвестен");
    }

    const auto estimate = forecast.by_component.find(component_key);
    const bool has_forecast = estimate != forecast.by_component.end() && estimate->remaining_ms.has_value();
    if (has_forecast) {
        data.remaining_ms = *estimate->remaining_ms;
        data.eta_text = FormatCountdownDuration(*estimate->remaining_ms);
    } else {
        data.eta_text = QString::fromUtf8(u8"Нет прогноза");
    }

    const BatteryRuntimeConfidence confidence =
        estimate != forecast.by_component.end() ? estimate->confidence : BatteryRuntimeConfidence::Low;
    data.confidence_text = BadgeText(confidence, has_forecast);
    data.confidence_state = BadgeState(confidence, has_forecast);

    if (!has_visible_level) {
        data.note_text = QString::fromUtf8(u8"Сейчас наушник не активен.");
    } else if (!has_forecast) {
        data.note_text = QString::fromUtf8(u8"Нужно больше истории.");
    } else if (confidence == BatteryRuntimeConfidence::High) {
        data.note_text = QString::fromUtf8(u8"Прогноз уже можно считать надёжным.");
    } else if (confidence == BatteryRuntimeConfidence::Medium) {
        data.note_text = QString::fromUtf8(u8"Прогноз уже полезен, но ещё уточняется.");
    } else {
        data.note_text = QString::fromUtf8(u8"Для этого режима пока мало данных.");
    }

    return data;
}

QString ImbalanceSeverityText(const ImbalanceSummary& imbalance) {
    if (imbalance.shared_samples < 8) {
        return QString::fromUtf8(u8"Перекос: не оценён");
    }

    const double abs_diff = std::abs(imbalance.average_diff);
    if (abs_diff < 5.0) {
        return QString::fromUtf8(u8"Перекос: слабый");
    }
    if (abs_diff < 15.0) {
        return QString::fromUtf8(u8"Перекос: заметный");
    }
    return QString::fromUtf8(u8"Перекос: сильный");
}

QString ImbalanceDetailsText(const ImbalanceSummary& imbalance) {
    if (imbalance.shared_samples < 8) {
        return QString::fromUtf8(u8"Нужно больше общей истории по обоим наушникам.");
    }

    QStringList parts;
    parts << QString::fromUtf8(u8"Средняя разница: %1%")
                 .arg(QString::number(std::abs(imbalance.average_diff), 'f', 1));
    if (imbalance.latest_diff.has_value()) {
        parts << QString::fromUtf8(u8"Разница сейчас: %1%").arg(std::abs(*imbalance.latest_diff));
    }
    parts << QString::fromUtf8(u8"Максимальный разрыв: %1%").arg(imbalance.max_abs_gap);
    return parts.join(QString::fromUtf8(u8" · "));
}

QString PrimaryInsight(const ImbalanceSummary& imbalance) {
    if (imbalance.shared_samples < 8) {
        return QString::fromUtf8(u8"Перекос между наушниками пока рано оценивать.");
    }

    if (std::abs(imbalance.average_diff) < 5.0) {
        return QString::fromUtf8(u8"Сильного перекоса между наушниками нет.");
    }

    if (imbalance.average_diff > 0.0) {
        return QString::fromUtf8(u8"Правый наушник разряжается заметно быстрее.");
    }

    return QString::fromUtf8(u8"Левый наушник разряжается заметно быстрее.");
}

QString SecondaryInsight(const EarCardData& left, const EarCardData& right) {
    const bool left_high = left.confidence_state == QStringLiteral("high");
    const bool right_high = right.confidence_state == QStringLiteral("high");
    const bool left_has_forecast = left.remaining_ms.has_value();
    const bool right_has_forecast = right.remaining_ms.has_value();

    if (left_high && right_high) {
        return QString::fromUtf8(u8"Прогноз по обоим наушникам уже можно считать надёжным.");
    }
    if (left_high && !right_high) {
        return QString::fromUtf8(u8"Прогноз по левому уже можно считать надёжным. По правому пока мало данных.");
    }
    if (right_high && !left_high) {
        return QString::fromUtf8(u8"Прогноз по правому уже можно считать надёжным. По левому пока мало данных.");
    }
    if (left_has_forecast || right_has_forecast) {
        return QString::fromUtf8(u8"Прогноз уже работает, но пока предварительный.");
    }
    return QString::fromUtf8(u8"Прогноз недоступен — нужно больше истории.");
}

QString GenericForecastHeadline(const QString& component_label, const EarCardData& data) {
    if (!data.remaining_ms.has_value()) {
        return QString::fromUtf8(u8"Для %1 пока мало данных для прогноза.").arg(component_label.toLower());
    }
    if (data.confidence_state == QStringLiteral("high")) {
        return QString::fromUtf8(u8"Прогноз по %1 уже можно считать надёжным.").arg(component_label.toLower());
    }
    if (data.confidence_state == QStringLiteral("medium")) {
        return QString::fromUtf8(u8"Прогноз по %1 уже полезен, но ещё уточняется.").arg(component_label.toLower());
    }
    return QString::fromUtf8(u8"Для %1 пока нужно больше истории.").arg(component_label.toLower());
}

QString GenericForecastDetails(const QString& component_label, const EarCardData& data) {
    if (!data.remaining_ms.has_value()) {
        return QString::fromUtf8(u8"%1: прогноз пока недоступен.").arg(component_label);
    }
    if (data.confidence_state == QStringLiteral("high")) {
        return QString::fromUtf8(u8"%1: прогноз уверенный.").arg(component_label);
    }
    return QString::fromUtf8(u8"%1: прогноз предварительный.").arg(component_label);
}

QString ScenarioRiskInsight(const BatteryRuntimeForecast& forecast) {
    const auto left_it = forecast.by_component.find(QStringLiteral("left"));
    const auto right_it = forecast.by_component.find(QStringLiteral("right"));
    const bool has_left = left_it != forecast.by_component.end() && left_it->remaining_ms.has_value();
    const bool has_right = right_it != forecast.by_component.end() && right_it->remaining_ms.has_value();

    if (!has_left && !has_right) {
        return QString::fromUtf8(u8"Для выбранного сценария пока мало данных.");
    }
    if (has_left && !has_right) {
        return QString::fromUtf8(u8"По выбранному сценарию прогноз есть только для левого.");
    }
    if (!has_left && has_right) {
        return QString::fromUtf8(u8"По выбранному сценарию прогноз есть только для правого.");
    }

    const qint64 left_ms = *left_it->remaining_ms;
    const qint64 right_ms = *right_it->remaining_ms;
    const qint64 gap_ms = std::llabs(left_ms - right_ms);

    if (gap_ms < 15LL * 60LL * 1000LL) {
        return QString::fromUtf8(u8"В выбранном сценарии наушники разрядятся примерно одновременно.");
    }
    if (left_ms < right_ms) {
        return QString::fromUtf8(u8"В выбранном сценарии левый окажется в зоне риска раньше.");
    }
    return QString::fromUtf8(u8"В выбранном сценарии правый окажется в зоне риска раньше.");
}

QLabel* MakeCardLabel(const char* object_name, QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setObjectName(QString::fromLatin1(object_name));
    label->setWordWrap(true);
    return label;
}

QFrame* CreateEarCard(const QString& title,
                      QLabel** title_label,
                      QLabel** percent_label,
                      QLabel** eta_label,
                      QLabel** confidence_badge,
                      QLabel** note_label,
                      QWidget* parent) {
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("statsCard"));
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(8);

    *title_label = new QLabel(title, card);
    (*title_label)->setObjectName(QStringLiteral("statsCardTitle"));

    *percent_label = MakeCardLabel("statsCardPercent", card);
    *eta_label = MakeCardLabel("statsCardEta", card);
    *confidence_badge = MakeCardLabel("statsBadge", card);
    *note_label = MakeCardLabel("statsCardNote", card);

    layout->addWidget(*title_label);
    layout->addWidget(*percent_label);
    layout->addWidget(*eta_label);
    layout->addWidget(*confidence_badge, 0, Qt::AlignLeft);
    layout->addWidget(*note_label);
    layout->addStretch(1);

    return card;
}

void ApplyBadgeState(QLabel* badge, const QString& state) {
    if (badge == nullptr) {
        return;
    }
    badge->setProperty("badgeState", state);
    badge->style()->unpolish(badge);
    badge->style()->polish(badge);
    badge->update();
}

}  // namespace

BatteryStatsDialog::BatteryStatsDialog(BatteryHistoryData history, QWidget* parent)
    : QDialog(parent), history_(std::move(history)) {
    setObjectName(QStringLiteral("batteryStatsDialog"));
    setWindowTitle(QString::fromUtf8(u8"Статистика"));
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(760, 620);
    setMinimumSize(680, 540);

    setStyleSheet(R"(
QDialog#batteryStatsDialog {
    background: #3B3E44;
    color: #F7F7F7;
    border: 1px solid rgba(255, 255, 255, 0.09);
    border-radius: 14px;
}
QLabel#statsTitle {
    color: #F8FAFC;
    font-size: 18px;
    font-weight: 700;
}
QLabel#statsMeta {
    color: #B7C0CE;
    font-size: 11px;
}
QLabel#statsMode {
    color: #F8FAFC;
    font-size: 14px;
    font-weight: 700;
}
QComboBox#statsCombo {
    background: #44484F;
    color: #F8FAFC;
    border: 1px solid rgba(255, 255, 255, 0.14);
    border-radius: 9px;
    padding: 4px 10px;
    min-height: 28px;
    min-width: 128px;
}
QComboBox#statsCombo QAbstractItemView {
    background: #2A2F37;
    color: #F8FAFC;
    selection-background-color: #50555E;
    border: 1px solid rgba(255,255,255,0.12);
}
QFrame#statsHero,
QFrame#statsConclusion,
QFrame#statsDetailsContainer {
    background: #2A2F37;
    border: 1px solid rgba(255, 255, 255, 0.10);
    border-radius: 12px;
}
QFrame#statsCard {
    background: #343A43;
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 12px;
}
QLabel#statsCardTitle {
    color: #AEB9C8;
    font-size: 12px;
    font-weight: 600;
}
QLabel#statsCardPercent {
    color: #D6DEEA;
    font-size: 18px;
    font-weight: 700;
}
QLabel#statsCardEta {
    color: #F8FAFC;
    font-size: 28px;
    font-weight: 800;
}
QLabel#statsCardNote {
    color: #B7C0CE;
    font-size: 11px;
    line-height: 1.4;
}
QLabel#statsBadge {
    color: #F8FAFC;
    font-size: 11px;
    font-weight: 700;
    padding: 4px 10px;
    border-radius: 999px;
}
QLabel#statsBadge[badgeState="high"] {
    background: rgba(48, 194, 110, 0.18);
    border: 1px solid rgba(48, 194, 110, 0.45);
}
QLabel#statsBadge[badgeState="medium"] {
    background: rgba(215, 180, 70, 0.18);
    border: 1px solid rgba(215, 180, 70, 0.45);
}
QLabel#statsBadge[badgeState="low"] {
    background: rgba(143, 153, 168, 0.18);
    border: 1px solid rgba(143, 153, 168, 0.38);
}
QLabel#statsSectionTitle {
    color: #F8FAFC;
    font-size: 14px;
    font-weight: 700;
}
QLabel#statsImbalance {
    color: #F8FAFC;
    font-size: 13px;
    font-weight: 700;
}
QLabel#statsImbalanceDetails,
QLabel#statsInsight {
    color: #D6DEEA;
    font-size: 12px;
    line-height: 1.45;
}
QToolButton#statsDetailsToggle {
    background: #44484F;
    color: #F8FAFC;
    border: 1px solid rgba(255, 255, 255, 0.14);
    border-radius: 10px;
    padding: 6px 12px;
    font-size: 11px;
    font-weight: 600;
}
QToolButton#statsDetailsToggle:hover {
    background: #50555E;
}
QTextBrowser#statsDetailsView {
    background: transparent;
    color: #AEB9C8;
    border: none;
    font-size: 12px;
}
QPushButton#statsCloseButton {
    background: #44484F;
    color: #F8FAFC;
    border: 1px solid rgba(255, 255, 255, 0.14);
    border-radius: 10px;
    padding: 6px 12px;
    font-size: 11px;
    font-weight: 600;
}
QPushButton#statsCloseButton:hover {
    background: #50555E;
}
)");

    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(18, 18, 18, 16);
    root_layout->setSpacing(12);

    auto* hero_frame = new QFrame(this);
    hero_frame->setObjectName(QStringLiteral("statsHero"));
    auto* hero_layout = new QVBoxLayout(hero_frame);
    hero_layout->setContentsMargins(16, 16, 16, 16);
    hero_layout->setSpacing(6);

    title_label_ = new QLabel(hero_frame);
    title_label_->setObjectName(QStringLiteral("statsTitle"));

    history_meta_label_ = new QLabel(hero_frame);
    history_meta_label_->setObjectName(QStringLiteral("statsMeta"));
    history_meta_label_->setWordWrap(true);

    mode_label_ = new QLabel(hero_frame);
    mode_label_->setObjectName(QStringLiteral("statsMode"));
    mode_label_->setWordWrap(true);

    scenario_label_ = new QLabel(hero_frame);
    scenario_label_->setObjectName(QStringLiteral("statsMeta"));
    scenario_label_->setWordWrap(true);

    scenario_controls_widget_ = new QWidget(hero_frame);
    auto* scenario_row = new QHBoxLayout(scenario_controls_widget_);
    scenario_row->setContentsMargins(0, 0, 0, 0);
    scenario_row->setSpacing(8);

    auto* scenario_title = new QLabel(QString::fromUtf8(u8"Смотреть как:"), hero_frame);
    scenario_title->setObjectName(QStringLiteral("statsMeta"));

    scenario_mode_combo_ = new QComboBox(hero_frame);
    scenario_mode_combo_->setObjectName(QStringLiteral("statsCombo"));

    scenario_submode_combo_ = new QComboBox(hero_frame);
    scenario_submode_combo_->setObjectName(QStringLiteral("statsCombo"));

    scenario_row->addWidget(scenario_title, 0);
    scenario_row->addWidget(scenario_mode_combo_, 0);
    scenario_row->addWidget(scenario_submode_combo_, 0);
    scenario_row->addStretch(1);

    connect(scenario_mode_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (updating_scenario_controls_) {
            return;
        }
        selected_scenario_mode_ = scenario_mode_combo_->currentData().toString();
        selected_scenario_submode_.clear();
        RefreshUi();
    });
    connect(scenario_submode_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (updating_scenario_controls_) {
            return;
        }
        selected_scenario_submode_ = scenario_submode_combo_->currentData().toString();
        RefreshUi();
    });

    hero_layout->addWidget(title_label_);
    hero_layout->addWidget(history_meta_label_);
    hero_layout->addWidget(mode_label_);
    hero_layout->addWidget(scenario_controls_widget_);
    hero_layout->addWidget(scenario_label_);

    auto* cards_row = new QHBoxLayout();
    cards_row->setContentsMargins(0, 0, 0, 0);
    cards_row->setSpacing(14);
    left_card_ = CreateEarCard(QString::fromUtf8(u8"Левый"),
                               &left_title_label_,
                               &left_percent_label_,
                               &left_eta_label_,
                               &left_confidence_badge_,
                               &left_note_label_,
                               this);
    right_card_ = CreateEarCard(QString::fromUtf8(u8"Правый"),
                                &right_title_label_,
                                &right_percent_label_,
                                &right_eta_label_,
                                &right_confidence_badge_,
                                &right_note_label_,
                                this);
    cards_row->addWidget(left_card_);
    cards_row->addWidget(right_card_);

    auto* conclusion_frame = new QFrame(this);
    conclusion_frame->setObjectName(QStringLiteral("statsConclusion"));
    auto* conclusion_layout = new QVBoxLayout(conclusion_frame);
    conclusion_layout->setContentsMargins(16, 16, 16, 16);
    conclusion_layout->setSpacing(8);

    auto* conclusion_title = new QLabel(QString::fromUtf8(u8"Вывод"), conclusion_frame);
    conclusion_title->setObjectName(QStringLiteral("statsSectionTitle"));

    imbalance_label_ = new QLabel(conclusion_frame);
    imbalance_label_->setObjectName(QStringLiteral("statsImbalance"));
    imbalance_details_label_ = new QLabel(conclusion_frame);
    imbalance_details_label_->setObjectName(QStringLiteral("statsImbalanceDetails"));
    imbalance_details_label_->setWordWrap(true);

    primary_insight_label_ = new QLabel(conclusion_frame);
    primary_insight_label_->setObjectName(QStringLiteral("statsInsight"));
    primary_insight_label_->setWordWrap(true);

    secondary_insight_label_ = new QLabel(conclusion_frame);
    secondary_insight_label_->setObjectName(QStringLiteral("statsInsight"));
    secondary_insight_label_->setWordWrap(true);

    conclusion_layout->addWidget(conclusion_title);
    conclusion_layout->addWidget(imbalance_label_);
    conclusion_layout->addWidget(imbalance_details_label_);
    conclusion_layout->addWidget(primary_insight_label_);
    conclusion_layout->addWidget(secondary_insight_label_);

    details_toggle_button_ = new QToolButton(this);
    details_toggle_button_->setObjectName(QStringLiteral("statsDetailsToggle"));
    details_toggle_button_->setText(QString::fromUtf8(u8"Подробности"));
    details_toggle_button_->setCheckable(true);
    details_toggle_button_->setChecked(false);
    details_toggle_button_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    details_toggle_button_->setArrowType(Qt::RightArrow);

    details_container_ = new QFrame(this);
    details_container_->setObjectName(QStringLiteral("statsDetailsContainer"));
    details_container_->setVisible(false);
    auto* details_layout = new QVBoxLayout(details_container_);
    details_layout->setContentsMargins(12, 12, 12, 12);
    details_layout->setSpacing(8);

    details_view_ = new QTextBrowser(details_container_);
    details_view_->setObjectName(QStringLiteral("statsDetailsView"));
    details_view_->setOpenLinks(false);
    details_view_->setOpenExternalLinks(false);
    details_layout->addWidget(details_view_);

    connect(details_toggle_button_, &QToolButton::toggled, this, [this](bool checked) {
        details_toggle_button_->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
        details_container_->setVisible(checked);
        adjustSize();
    });

    auto* button_row = new QHBoxLayout();
    button_row->setContentsMargins(0, 0, 0, 0);
    button_row->addStretch(1);

    auto* close_button = new QPushButton(QString::fromUtf8(u8"Закрыть"), this);
    close_button->setObjectName(QStringLiteral("statsCloseButton"));
    connect(close_button, &QPushButton::clicked, this, &QDialog::close);
    button_row->addWidget(close_button);

    countdown_timer_ = new QTimer(this);
    countdown_timer_->setInterval(1000);
    connect(countdown_timer_, &QTimer::timeout, this, &BatteryStatsDialog::UpdateCountdownLabels);

    root_layout->addWidget(hero_frame);
    root_layout->addLayout(cards_row);
    root_layout->addWidget(conclusion_frame);
    root_layout->addWidget(details_toggle_button_, 0, Qt::AlignLeft);
    root_layout->addWidget(details_container_);
    root_layout->addLayout(button_row);

    RefreshUi();
}

void BatteryStatsDialog::SetHistory(BatteryHistoryData history) {
    history_ = std::move(history);
    RefreshUi();
}

void BatteryStatsDialog::UpdateCountdownLabels() {
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();

    if (left_countdown_deadline_ms_.has_value()) {
        left_eta_label_->setText(
            FormatCountdownDuration(std::max<qint64>(0, *left_countdown_deadline_ms_ - now_ms)));
    }
    if (right_countdown_deadline_ms_.has_value()) {
        right_eta_label_->setText(
            FormatCountdownDuration(std::max<qint64>(0, *right_countdown_deadline_ms_ - now_ms)));
    }
}

void BatteryStatsDialog::RefreshUi() {
    title_label_->setText(DialogTitle(history_));
    history_meta_label_->setText(HistoryMeta(history_));
    mode_label_->setText(CurrentModeText(history_));

    const QList<QString> display_components = DisplayComponents(history_);
    const QString primary_component =
        display_components.isEmpty() ? QStringLiteral("main") : display_components.front();
    const QString secondary_component = display_components.size() > 1 ? display_components[1] : QString();
    const bool stereo_layout = HasStereoEarbudsLayout(display_components);
    const bool has_mode_data = HasModeData(history_);

    if (left_title_label_ != nullptr) {
        left_title_label_->setText(DisplayComponentLabel(primary_component));
    }
    if (right_title_label_ != nullptr) {
        right_title_label_->setText(DisplayComponentLabel(secondary_component));
    }
    if (left_card_ != nullptr) {
        left_card_->setVisible(true);
    }
    if (right_card_ != nullptr) {
        right_card_->setVisible(!secondary_component.isEmpty());
    }
    if (mode_label_ != nullptr) {
        mode_label_->setVisible(has_mode_data);
    }
    if (scenario_controls_widget_ != nullptr) {
        scenario_controls_widget_->setVisible(has_mode_data);
    }
    if (scenario_label_ != nullptr) {
        scenario_label_->setVisible(has_mode_data);
    }
    if (!has_mode_data) {
        selected_scenario_mode_ = QStringLiteral("current");
        selected_scenario_submode_.clear();
    }

    const QString actual_mode = ActualModeToken(history_);
    const QString actual_submode = ActualSubmodeToken(history_);

    updating_scenario_controls_ = true;
    const QString previous_mode = selected_scenario_mode_;
    const QString previous_submode = selected_scenario_submode_;

    scenario_mode_combo_->clear();
    scenario_mode_combo_->addItem(QString::fromUtf8(u8"Текущий"), QStringLiteral("current"));
    scenario_mode_combo_->addItem(QString::fromUtf8(u8"Выкл"), QStringLiteral("off"));
    scenario_mode_combo_->addItem(QString::fromUtf8(u8"ANC"), QStringLiteral("anc"));
    scenario_mode_combo_->addItem(QString::fromUtf8(u8"Прозрачность"), QStringLiteral("transparency"));

    int mode_index = scenario_mode_combo_->findData(previous_mode);
    if (mode_index < 0) {
        mode_index = 0;
    }
    scenario_mode_combo_->setCurrentIndex(mode_index);
    selected_scenario_mode_ = scenario_mode_combo_->currentData().toString();

    scenario_submode_combo_->clear();
    if (ScenarioNeedsSubmode(selected_scenario_mode_)) {
        if (selected_scenario_mode_ == QStringLiteral("anc")) {
            scenario_submode_combo_->addItem(QString::fromUtf8(u8"Баланс"), QStringLiteral("balanced"));
            scenario_submode_combo_->addItem(QString::fromUtf8(u8"Слабое"), QStringLiteral("weak"));
            scenario_submode_combo_->addItem(QString::fromUtf8(u8"Глубокое"), QStringLiteral("deep"));
            scenario_submode_combo_->addItem(QString::fromUtf8(u8"Адаптивное"), QStringLiteral("adaptive"));
        } else {
            scenario_submode_combo_->addItem(QString::fromUtf8(u8"Обычная"), QStringLiteral("standard"));
            scenario_submode_combo_->addItem(QString::fromUtf8(u8"Голос"), QStringLiteral("voice"));
        }

        QString preferred_submode = previous_submode;
        if (preferred_submode.isEmpty() && selected_scenario_mode_ == actual_mode && !actual_submode.isEmpty()) {
            preferred_submode = actual_submode;
        }
        if (preferred_submode.isEmpty()) {
            preferred_submode = DefaultSubmodeForMode(selected_scenario_mode_);
        }

        int submode_index = scenario_submode_combo_->findData(preferred_submode);
        if (submode_index < 0) {
            submode_index = 0;
        }
        scenario_submode_combo_->setCurrentIndex(submode_index);
        selected_scenario_submode_ = scenario_submode_combo_->currentData().toString();
        scenario_submode_combo_->setVisible(true);
    } else {
        selected_scenario_submode_.clear();
        scenario_submode_combo_->setVisible(false);
    }
    updating_scenario_controls_ = false;

    scenario_label_->setText(
        ScenarioText(selected_scenario_mode_ == QStringLiteral("current") ? actual_mode : selected_scenario_mode_,
                     selected_scenario_mode_ == QStringLiteral("current") ? actual_submode : selected_scenario_submode_,
                     selected_scenario_mode_ == QStringLiteral("current")));

    const QString effective_mode =
        selected_scenario_mode_ == QStringLiteral("current") ? actual_mode : selected_scenario_mode_;
    const QString effective_submode =
        selected_scenario_mode_ == QStringLiteral("current") ? actual_submode : selected_scenario_submode_;

    if (history_.samples.isEmpty()) {
        left_countdown_deadline_ms_.reset();
        right_countdown_deadline_ms_.reset();
        left_countdown_state_key_.clear();
        right_countdown_state_key_.clear();
        if (countdown_timer_ != nullptr) {
            countdown_timer_->stop();
        }
        const QString no_history = QString::fromUtf8(u8"Текущая история ещё не накопилась.");
        left_percent_label_->setText(QString::fromUtf8(u8"Заряд неизвестен"));
        left_eta_label_->setText(QString::fromUtf8(u8"Нет прогноза"));
        left_confidence_badge_->setText(QString::fromUtf8(u8"Недостаточно данных"));
        left_note_label_->setText(QString::fromUtf8(u8"Нужно больше истории."));
        ApplyBadgeState(left_confidence_badge_, QStringLiteral("low"));

        right_percent_label_->setText(QString::fromUtf8(u8"Заряд неизвестен"));
        right_eta_label_->setText(QString::fromUtf8(u8"Нет прогноза"));
        right_confidence_badge_->setText(QString::fromUtf8(u8"Недостаточно данных"));
        right_note_label_->setText(QString::fromUtf8(u8"Нужно больше истории."));
        ApplyBadgeState(right_confidence_badge_, QStringLiteral("low"));

        imbalance_label_->setText(stereo_layout ? QString::fromUtf8(u8"Перекос: не оценён")
                                                : QString::fromUtf8(u8"Оценка"));
        imbalance_details_label_->setText(
            stereo_layout ? QString::fromUtf8(u8"Нужно больше общей истории по обоим наушникам.")
                          : QString::fromUtf8(u8"Нужно накопить больше истории по устройству."));
        primary_insight_label_->setText(no_history);
        secondary_insight_label_->clear();
        details_view_->setPlainText(BuildBatteryStatisticsReport(history_));
        return;
    }

    const BatteryHistoryData scenario_history =
        BuildScenarioHistory(history_, selected_scenario_mode_, selected_scenario_submode_);
    const auto forecast = EstimateBatteryRuntimeForecast(scenario_history);
    const auto left_data = BuildEarCardData(scenario_history, forecast, primary_component);
    const auto right_data = secondary_component.isEmpty()
                                ? EarCardData{}
                                : BuildEarCardData(scenario_history, forecast, secondary_component);
    const auto imbalance = ComputeImbalanceSummary(history_);

    left_percent_label_->setText(left_data.percent_text);
    left_eta_label_->setText(left_data.eta_text);
    left_confidence_badge_->setText(left_data.confidence_text);
    left_note_label_->setText(left_data.note_text);
    ApplyBadgeState(left_confidence_badge_, left_data.confidence_state);

    if (!secondary_component.isEmpty()) {
        right_percent_label_->setText(right_data.percent_text);
        right_eta_label_->setText(right_data.eta_text);
        right_confidence_badge_->setText(right_data.confidence_text);
        right_note_label_->setText(right_data.note_text);
        ApplyBadgeState(right_confidence_badge_, right_data.confidence_state);
    }

    const qint64 latest_timestamp_ms = scenario_history.samples.back().timestamp_ms;
    const auto left_level = SampleLevel(scenario_history.samples.back(), primary_component);
    const QString left_state_key = BuildCountdownStateKey(primary_component, left_level, effective_mode, effective_submode);
    const std::optional<qint64> proposed_left_deadline =
        left_data.remaining_ms.has_value() ? std::optional<qint64>(latest_timestamp_ms + *left_data.remaining_ms)
                                           : std::nullopt;
    if (left_state_key == left_countdown_state_key_ && left_countdown_deadline_ms_.has_value()) {
        if (proposed_left_deadline.has_value()) {
            left_countdown_deadline_ms_ = std::min(*left_countdown_deadline_ms_, *proposed_left_deadline);
        }
    } else {
        left_countdown_deadline_ms_ = proposed_left_deadline;
        left_countdown_state_key_ = left_state_key;
    }

    const auto right_level =
        secondary_component.isEmpty() ? std::optional<int>{} : SampleLevel(scenario_history.samples.back(), secondary_component);
    const QString right_state_key = secondary_component.isEmpty()
                                        ? QString()
                                        : BuildCountdownStateKey(secondary_component, right_level, effective_mode,
                                                                 effective_submode);
    const std::optional<qint64> proposed_right_deadline =
        (!secondary_component.isEmpty() && right_data.remaining_ms.has_value())
            ? std::optional<qint64>(latest_timestamp_ms + *right_data.remaining_ms)
            : std::nullopt;
    if (secondary_component.isEmpty()) {
        right_countdown_deadline_ms_.reset();
        right_countdown_state_key_.clear();
    } else if (right_state_key == right_countdown_state_key_ && right_countdown_deadline_ms_.has_value()) {
        if (proposed_right_deadline.has_value()) {
            right_countdown_deadline_ms_ = std::min(*right_countdown_deadline_ms_, *proposed_right_deadline);
        }
    } else {
        right_countdown_deadline_ms_ = proposed_right_deadline;
        right_countdown_state_key_ = right_state_key;
    }
    if (left_countdown_deadline_ms_.has_value() || right_countdown_deadline_ms_.has_value()) {
        if (countdown_timer_ != nullptr) {
            countdown_timer_->start();
        }
        UpdateCountdownLabels();
    } else if (countdown_timer_ != nullptr) {
        countdown_timer_->stop();
    }

    if (stereo_layout) {
        imbalance_label_->setText(ImbalanceSeverityText(imbalance));
        imbalance_details_label_->setText(ImbalanceDetailsText(imbalance));
        primary_insight_label_->setText(ScenarioRiskInsight(forecast));
        secondary_insight_label_->setText(SecondaryInsight(left_data, right_data));
    } else {
        const QString primary_label = DisplayComponentLabel(primary_component);
        imbalance_label_->setText(QString::fromUtf8(u8"Оценка"));
        imbalance_details_label_->setText(
            primary_component == QStringLiteral("main")
                ? QString::fromUtf8(u8"Прогноз строится по общему заряду устройства.")
                : QString::fromUtf8(u8"Прогноз строится по компоненту «%1».").arg(primary_label));
        primary_insight_label_->setText(GenericForecastHeadline(primary_label, left_data));
        secondary_insight_label_->setText(
            secondary_component.isEmpty()
                ? QString()
                : GenericForecastDetails(DisplayComponentLabel(secondary_component), right_data));
    }

    details_view_->setPlainText(BuildBatteryStatisticsReport(scenario_history));
}

}  // namespace battery_monitor
