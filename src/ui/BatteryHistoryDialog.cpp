#include "ui/BatteryHistoryDialog.h"
#include "ui/BatteryRuntimeEstimator.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWidget>

namespace battery_monitor {

namespace {

struct HistorySeries {
    QString component_key;
    QVector<QPointF> points;
    QVector<qint64> timestamps_ms;
    QVector<int> levels;
};

struct HoverValue {
    QString component_key;
    QPointF point;
    double level = 0.0;
};

struct TimelineLayout {
    QVector<qint64> timestamps_ms;
    QVector<qreal> x_positions;
};

constexpr int kHistoryChartLeftPadding = 48;
constexpr int kHistoryChartTopPadding = 16;
constexpr int kHistoryChartRightPadding = 18;
constexpr int kHistoryChartBottomPadding = 34;
constexpr qint64 kDayRangeMs = 24LL * 60LL * 60LL * 1000LL;
constexpr qint64 kCompressedGapCapMs = 20LL * 60LL * 1000LL;
constexpr qint64 kSeriesGapBreakMs = 45LL * 60LL * 1000LL;
constexpr int kChargeJumpBreakPercent = 3;

QString ComponentLabel(const QString& component_key) {
    if (component_key == QStringLiteral("left")) {
        return QString::fromUtf8(u8"Левый");
    }
    if (component_key == QStringLiteral("right")) {
        return QString::fromUtf8(u8"Правый");
    }
    if (component_key == QStringLiteral("case")) {
        return QString::fromUtf8(u8"Кейс");
    }
    if (component_key == QStringLiteral("main")) {
        return QString::fromUtf8(u8"Заряд");
    }
    if (component_key.isEmpty()) {
        return QString::fromUtf8(u8"Компонент");
    }

    QString label = component_key;
    label[0] = label.at(0).toUpper();
    return label;
}

QColor ComponentColor(const QString& component_key) {
    if (component_key == QStringLiteral("left")) {
        return QColor(QStringLiteral("#74BEFF"));
    }
    if (component_key == QStringLiteral("right")) {
        return QColor(QStringLiteral("#30C26E"));
    }
    if (component_key == QStringLiteral("case")) {
        return QColor(QStringLiteral("#D7B446"));
    }
    if (component_key == QStringLiteral("main")) {
        return QColor(QStringLiteral("#E7ECF6"));
    }

    const std::array<QColor, 4> fallback_colors = {
        QColor(QStringLiteral("#A7B0C0")),
        QColor(QStringLiteral("#E06767")),
        QColor(QStringLiteral("#8892A2")),
        QColor(QStringLiteral("#5F6876")),
    };
    const std::size_t color_index = static_cast<std::size_t>(qHash(component_key)) % fallback_colors.size();
    return fallback_colors[color_index];
}

int ComponentOrder(const QString& component_key) {
    if (component_key == QStringLiteral("left")) {
        return 0;
    }
    if (component_key == QStringLiteral("right")) {
        return 1;
    }
    if (component_key == QStringLiteral("case")) {
        return 2;
    }
    if (component_key == QStringLiteral("main")) {
        return 3;
    }
    return 10;
}

bool IsDisconnectedLevel(int level) {
    return level <= 0;
}

bool IsSeriesBreak(const HistorySeries& series, int left_index, int right_index) {
    if (left_index < 0 || right_index < 0 ||
        left_index >= series.levels.size() || right_index >= series.levels.size()) {
        return true;
    }

    if (IsDisconnectedLevel(series.levels[left_index]) || IsDisconnectedLevel(series.levels[right_index])) {
        return true;
    }

    if ((series.timestamps_ms[right_index] - series.timestamps_ms[left_index]) > kSeriesGapBreakMs) {
        return true;
    }

    return (series.levels[right_index] - series.levels[left_index]) > kChargeJumpBreakPercent;
}

QString FormatFullTimestamp(qint64 timestamp_ms) {
    return QLocale::system().toString(QDateTime::fromMSecsSinceEpoch(timestamp_ms),
                                      QStringLiteral("dd MMM yyyy HH:mm"));
}

QString FormatDayLabel(const QDate& day) {
    if (!day.isValid()) {
        return QString::fromUtf8(u8"Нет данных");
    }
    return QLocale::system().toString(day, QStringLiteral("dd MMM yyyy"));
}

QString FormatAxisLabel(qint64 timestamp_ms) {
    return QLocale::system().toString(QDateTime::fromMSecsSinceEpoch(timestamp_ms).time(), QStringLiteral("HH:mm"));
}

QString ModeLabel(const QString& mode) {
    const QString normalized = mode.trimmed().toLower();
    if (normalized == QStringLiteral("off")) {
        return QString::fromUtf8(u8"выключено");
    }
    if (normalized == QStringLiteral("anc")) {
        return QString::fromUtf8(u8"шумоподавление");
    }
    if (normalized == QStringLiteral("transparency")) {
        return QString::fromUtf8(u8"прозрачность");
    }
    return normalized;
}

QString SubmodeLabel(const QString& submode) {
    const QString normalized = submode.trimmed().toLower();
    if (normalized == QStringLiteral("balance")) {
        return QString::fromUtf8(u8"баланс");
    }
    if (normalized == QStringLiteral("weak")) {
        return QString::fromUtf8(u8"слабое");
    }
    if (normalized == QStringLiteral("deep")) {
        return QString::fromUtf8(u8"глубокое");
    }
    if (normalized == QStringLiteral("adaptive")) {
        return QString::fromUtf8(u8"адаптивное");
    }
    if (normalized == QStringLiteral("normal")) {
        return QString::fromUtf8(u8"обычная");
    }
    if (normalized == QStringLiteral("voice")) {
        return QString::fromUtf8(u8"усиление голоса");
    }
    return normalized;
}

qint64 DayWindowStartMs(const QDate& day) {
    return QDateTime(day, QTime(0, 0)).toMSecsSinceEpoch();
}

qint64 DayWindowEndMs(const QDate& day) {
    return DayWindowStartMs(day) + kDayRangeMs;
}

QString BuildDialogTitle(const BatteryHistoryData& history) {
    if (!history.device_name.trimmed().isEmpty()) {
        return history.device_name;
    }
    if (!history.device_id.trimmed().isEmpty()) {
        return history.device_id;
    }
    return QString::fromUtf8(u8"История батареи");
}

QVector<QDate> AvailableDays(const BatteryHistoryData& history) {
    QVector<QDate> days;
    for (const auto& sample : history.samples) {
        const QDate day = QDateTime::fromMSecsSinceEpoch(sample.timestamp_ms).date();
        if (!days.contains(day)) {
            days.push_back(day);
        }
    }
    std::sort(days.begin(), days.end());
    return days;
}

QDate DefaultSelectedDay(const BatteryHistoryData& history) {
    const auto days = AvailableDays(history);
    if (!days.isEmpty()) {
        return days.back();
    }
    return QDate::currentDate();
}

BatteryHistoryData FilterHistoryForDay(const BatteryHistoryData& history, const QDate& day) {
    BatteryHistoryData filtered;
    filtered.device_id = history.device_id;
    filtered.device_name = history.device_name;

    if (!day.isValid()) {
        return filtered;
    }

    const qint64 start_ms = DayWindowStartMs(day);
    const qint64 end_ms = DayWindowEndMs(day);
    for (const auto& sample : history.samples) {
        if (sample.timestamp_ms >= start_ms && sample.timestamp_ms < end_ms) {
            filtered.samples.push_back(sample);
        }
    }
    return filtered;
}

BatteryHistoryData FilterHiddenComponents(const BatteryHistoryData& history, const QSet<QString>& hidden_components) {
    BatteryHistoryData filtered;
    filtered.device_id = history.device_id;
    filtered.device_name = history.device_name;

    for (const auto& sample : history.samples) {
        BatteryHistorySample filtered_sample;
        filtered_sample.timestamp_ms = sample.timestamp_ms;
        filtered_sample.device_mode = sample.device_mode;
        filtered_sample.device_submode = sample.device_submode;
        for (auto it = sample.component_levels.cbegin(); it != sample.component_levels.cend(); ++it) {
            if (!hidden_components.contains(it.key())) {
                filtered_sample.component_levels.insert(it.key(), it.value());
            }
        }
        filtered.samples.push_back(std::move(filtered_sample));
    }

    return filtered;
}

QString BuildDialogSubtitle(const BatteryHistoryData& full_history,
                            const BatteryHistoryData& visible_history,
                            const QDate& selected_day) {
    if (full_history.samples.isEmpty()) {
        return QString::fromUtf8(u8"История появится после нескольких живых обновлений.");
    }

    if (visible_history.samples.isEmpty()) {
        return QString::fromUtf8(u8"Период: %1 · за эти сутки точек пока нет.")
            .arg(FormatDayLabel(selected_day));
    }

    return QString::fromUtf8(u8"Период: %1 · точек: %2 · %3 - %4")
        .arg(FormatDayLabel(selected_day))
        .arg(visible_history.samples.size())
        .arg(FormatFullTimestamp(visible_history.samples.front().timestamp_ms))
        .arg(FormatFullTimestamp(visible_history.samples.back().timestamp_ms));
}

QString BuildHintText(const BatteryHistoryData& full_history) {
    if (full_history.samples.size() < 2) {
        return QString::fromUtf8(
            u8"График строится по живым значениям. После пары обновлений здесь появятся первые сутки истории.");
    }
    return QString::fromUtf8(
        u8"Показаны только 24 часа выбранного дня. Переключайте дни стрелками слева и справа.");
}

QStringList VisibleComponents(const BatteryHistoryData& history) {
    QStringList component_keys;
    for (const auto& sample : history.samples) {
        for (auto it = sample.component_levels.cbegin(); it != sample.component_levels.cend(); ++it) {
            if (!component_keys.contains(it.key())) {
                component_keys.push_back(it.key());
            }
        }
    }

    std::sort(component_keys.begin(), component_keys.end(), [](const QString& left, const QString& right) {
        if (ComponentOrder(left) != ComponentOrder(right)) {
            return ComponentOrder(left) < ComponentOrder(right);
        }
        return QString::localeAwareCompare(left, right) < 0;
    });
    return component_keys;
}

int DayIndex(const QVector<QDate>& days, const QDate& day) {
    for (int index = 0; index < days.size(); ++index) {
        if (days[index] == day) {
            return index;
        }
    }
    return -1;
}

TimelineLayout BuildCompressedTimeline(const BatteryHistoryData& history, const QRectF& chart_rect) {
    TimelineLayout layout;
    if (history.samples.isEmpty()) {
        return layout;
    }

    layout.timestamps_ms.reserve(history.samples.size());
    layout.x_positions.reserve(history.samples.size());
    for (const auto& sample : history.samples) {
        layout.timestamps_ms.push_back(sample.timestamp_ms);
    }

    if (history.samples.size() == 1) {
        layout.x_positions.push_back(chart_rect.center().x());
        return layout;
    }

    QVector<qint64> compressed_offsets;
    compressed_offsets.reserve(history.samples.size());
    compressed_offsets.push_back(0);

    qint64 compressed_total = 0;
    for (int index = 1; index < history.samples.size(); ++index) {
        const qint64 raw_delta =
            std::max<qint64>(1, history.samples[index].timestamp_ms - history.samples[index - 1].timestamp_ms);
        compressed_total += std::min(raw_delta, kCompressedGapCapMs);
        compressed_offsets.push_back(compressed_total);
    }

    const qint64 normalized_total = std::max<qint64>(1, compressed_total);
    for (const qint64 offset : compressed_offsets) {
        const qreal progress = static_cast<qreal>(offset) / static_cast<qreal>(normalized_total);
        layout.x_positions.push_back(chart_rect.left() + (progress * chart_rect.width()));
    }

    return layout;
}

qint64 InterpolateTimestampAtX(const TimelineLayout& timeline, qreal x) {
    if (timeline.timestamps_ms.isEmpty() || timeline.x_positions.isEmpty()) {
        return 0;
    }
    if (timeline.timestamps_ms.size() == 1 || timeline.x_positions.size() == 1) {
        return timeline.timestamps_ms.front();
    }

    if (x <= timeline.x_positions.front()) {
        return timeline.timestamps_ms.front();
    }
    if (x >= timeline.x_positions.back()) {
        return timeline.timestamps_ms.back();
    }

    for (int index = 0; index + 1 < timeline.x_positions.size(); ++index) {
        const qreal left_x = timeline.x_positions[index];
        const qreal right_x = timeline.x_positions[index + 1];
        if (x < left_x || x > right_x) {
            continue;
        }

        if (qFuzzyCompare(left_x, right_x)) {
            return timeline.timestamps_ms[index];
        }

        const qreal progress = (x - left_x) / (right_x - left_x);
        return timeline.timestamps_ms[index] + static_cast<qint64>(
            (timeline.timestamps_ms[index + 1] - timeline.timestamps_ms[index]) * progress);
    }

    return timeline.timestamps_ms.back();
}

QString FormatDurationCompact(qint64 duration_ms) {
    const qint64 total_minutes = std::max<qint64>(1, duration_ms / (60LL * 1000LL));
    const qint64 hours = total_minutes / 60LL;
    const qint64 minutes = total_minutes % 60LL;
    if (hours <= 0) {
        return QString::fromUtf8(u8"%1 м").arg(minutes);
    }
    if (minutes == 0) {
        return QString::fromUtf8(u8"%1 ч").arg(hours);
    }
    return QString::fromUtf8(u8"%1 ч %2 м").arg(hours).arg(minutes);
}

QString BuildRuntimeEstimateText(const BatteryHistoryData& history, const QSet<QString>& hidden_components) {
    QStringList parts;
    const auto component_keys = VisibleComponents(history);
    for (const auto& component_key : component_keys) {
        if (hidden_components.contains(component_key)) {
            continue;
        }

        qint64 total_duration_ms = 0;
        int total_drop = 0;
        bool have_previous = false;
        qint64 previous_timestamp_ms = 0;
        int previous_level = -1;

        for (const auto& sample : history.samples) {
            const auto level_it = sample.component_levels.find(component_key);
            const int current_level = level_it == sample.component_levels.end() ? -1 : level_it.value();

            if (!have_previous) {
                previous_timestamp_ms = sample.timestamp_ms;
                previous_level = current_level;
                have_previous = true;
                continue;
            }

            const qint64 delta_ms = sample.timestamp_ms - previous_timestamp_ms;
            const bool invalid_pair = delta_ms <= 0 ||
                                      previous_level < 0 || current_level < 0 ||
                                      IsDisconnectedLevel(previous_level) || IsDisconnectedLevel(current_level) ||
                                      delta_ms > kSeriesGapBreakMs ||
                                      (current_level - previous_level) > kChargeJumpBreakPercent;

            if (!invalid_pair) {
                total_duration_ms += delta_ms;
                total_drop += std::max(0, previous_level - current_level);
            }

            previous_timestamp_ms = sample.timestamp_ms;
            previous_level = current_level;
        }

        if (total_duration_ms <= 0 || total_drop < 10) {
            continue;
        }

        const qint64 estimated_full_ms =
            static_cast<qint64>((static_cast<long double>(total_duration_ms) * 100.0L) /
                                static_cast<long double>(total_drop));
        parts.push_back(QStringLiteral("%1 %2")
                            .arg(ComponentLabel(component_key))
                            .arg(FormatDurationCompact(estimated_full_ms)));
    }

    if (parts.isEmpty()) {
        return QString::fromUtf8(u8"Среднее время работы 100→0: недостаточно данных");
    }

    return QString::fromUtf8(u8"Среднее время работы 100→0: %1")
        .arg(parts.join(QString::fromUtf8(u8" · ")));
}

struct SummaryCardText {
    QString value;
    QString note;
};

std::optional<qint64> EstimateAverageRuntimeForComponent(const BatteryHistoryData& history,
                                                         const QString& component_key) {
    qint64 total_duration_ms = 0;
    int total_drop = 0;
    bool have_previous = false;
    qint64 previous_timestamp_ms = 0;
    int previous_level = -1;

    for (const auto& sample : history.samples) {
        const auto level_it = sample.component_levels.find(component_key);
        const int current_level = level_it == sample.component_levels.end() ? -1 : level_it.value();

        if (!have_previous) {
            previous_timestamp_ms = sample.timestamp_ms;
            previous_level = current_level;
            have_previous = true;
            continue;
        }

        const qint64 delta_ms = sample.timestamp_ms - previous_timestamp_ms;
        const bool invalid_pair = delta_ms <= 0 ||
                                  previous_level < 0 || current_level < 0 ||
                                  IsDisconnectedLevel(previous_level) || IsDisconnectedLevel(current_level) ||
                                  delta_ms > kSeriesGapBreakMs ||
                                  (current_level - previous_level) > kChargeJumpBreakPercent;

        if (!invalid_pair) {
            total_duration_ms += delta_ms;
            total_drop += std::max(0, previous_level - current_level);
        }

        previous_timestamp_ms = sample.timestamp_ms;
        previous_level = current_level;
    }

    if (total_duration_ms <= 0 || total_drop < 10) {
        return std::nullopt;
    }

    return static_cast<qint64>((static_cast<long double>(total_duration_ms) * 100.0L) /
                               static_cast<long double>(total_drop));
}

std::optional<qint64> RemainingMsForComponent(const QString& component_key,
                                              const QHash<QString, qint64>& component_runtime_deadlines_ms,
                                              std::optional<qint64> runtime_deadline_ms,
                                              const BatteryRuntimeForecast& forecast) {
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const auto deadline_it = component_runtime_deadlines_ms.find(component_key);
    if (deadline_it != component_runtime_deadlines_ms.end()) {
        return std::max<qint64>(0, deadline_it.value() - now_ms);
    }

    if (component_key == QStringLiteral("main") && runtime_deadline_ms.has_value()) {
        return std::max<qint64>(0, *runtime_deadline_ms - now_ms);
    }

    const auto forecast_it = forecast.by_component.find(component_key);
    if (forecast_it == forecast.by_component.end() || !forecast_it->remaining_ms.has_value()) {
        return std::nullopt;
    }
    return *forecast_it->remaining_ms;
}

SummaryCardText BuildComponentCard(const QString& component_key,
                                   const BatteryHistoryData& history,
                                   const QHash<QString, qint64>& component_runtime_deadlines_ms,
                                   std::optional<qint64> runtime_deadline_ms,
                                   const BatteryRuntimeForecast& forecast) {
    const auto remaining_ms = RemainingMsForComponent(component_key,
                                                      component_runtime_deadlines_ms,
                                                      runtime_deadline_ms,
                                                      forecast);
    const auto average_ms = EstimateAverageRuntimeForComponent(history, component_key);

    SummaryCardText card;
    if (remaining_ms.has_value()) {
        card.value = FormatRuntimeDurationCompact(*remaining_ms);
    } else {
        card.value = QString::fromUtf8(u8"—");
    }

    if (average_ms.has_value()) {
        card.note = QString::fromUtf8(u8"100→0: %1").arg(FormatDurationCompact(*average_ms));
    } else {
        card.note = QString::fromUtf8(u8"100→0: мало данных");
    }

    return card;
}

QString BuildCompactHistorySubtitle(const BatteryHistoryData& full_history,
                                    const BatteryHistoryData& visible_history,
                                    const QDate& selected_day) {
    if (full_history.samples.isEmpty()) {
        return QString::fromUtf8(u8"История появится после нескольких живых обновлений.");
    }
    if (visible_history.samples.isEmpty()) {
        return QString::fromUtf8(u8"История за %1 пока пуста.").arg(FormatDayLabel(selected_day));
    }
    const auto& latest_sample = full_history.samples.back();
    if (latest_sample.device_mode.trimmed().isEmpty()) {
        return QString::fromUtf8(u8"История за %1").arg(FormatDayLabel(selected_day));
    }

    QString mode_text = ModeLabel(latest_sample.device_mode);
    if (!latest_sample.device_submode.trimmed().isEmpty()) {
        mode_text += QString::fromUtf8(u8" · %1").arg(SubmodeLabel(latest_sample.device_submode));
    }
    return QString::fromUtf8(u8"История за %1 · сейчас: %2")
        .arg(FormatDayLabel(selected_day))
        .arg(mode_text);
}

QString BuildCompactStatsLine(const BatteryHistoryData& history) {
    Q_UNUSED(history);
    return QString();
}

QFrame* CreateSummaryCard(QLabel** title_label,
                          QLabel** value_label,
                          QLabel** note_label,
                          QWidget* parent) {
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("historySummaryCard"));
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(4);

    *title_label = new QLabel(card);
    (*title_label)->setObjectName(QStringLiteral("historySummaryTitle"));

    *value_label = new QLabel(card);
    (*value_label)->setObjectName(QStringLiteral("historySummaryValue"));

    *note_label = new QLabel(card);
    (*note_label)->setObjectName(QStringLiteral("historySummaryNote"));
    (*note_label)->setWordWrap(true);

    layout->addWidget(*title_label);
    layout->addWidget(*value_label);
    layout->addWidget(*note_label);

    return card;
}

}  // namespace

class BatteryHistoryChartWidget final : public QWidget {
   public:
    explicit BatteryHistoryChartWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumHeight(220);
        setMouseTracking(true);
    }

    void SetHistory(BatteryHistoryData history, qint64 window_start_ms, qint64 window_end_ms) {
        history_ = std::move(history);
        window_start_ms_ = window_start_ms;
        window_end_ms_ = std::max(window_start_ms_ + 1, window_end_ms);
        update();
    }

   protected:
    void mouseMoveEvent(QMouseEvent* event) override {
        const QRectF chart_rect = ChartRect();
        if (chart_rect.contains(event->position())) {
            hover_active_ = true;
            hover_x_ = std::clamp(event->position().x(), chart_rect.left(), chart_rect.right());
        } else {
            hover_active_ = false;
        }
        update();
        QWidget::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        hover_active_ = false;
        update();
        QWidget::leaveEvent(event);
    }

    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), Qt::transparent);

        const QRectF frame_rect = rect().adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(QPen(QColor(255, 255, 255, 23), 1.0));
        painter.setBrush(QColor(QStringLiteral("#2A2F37")));
        painter.drawRoundedRect(frame_rect, 14.0, 14.0);

        const QRectF chart_rect = frame_rect.adjusted(kHistoryChartLeftPadding, kHistoryChartTopPadding,
                                                      -kHistoryChartRightPadding, -kHistoryChartBottomPadding);
        if (chart_rect.width() < 10.0 || chart_rect.height() < 10.0) {
            return;
        }

        painter.setClipRect(chart_rect.adjusted(-16.0, -16.0, 16.0, 16.0));
        painter.setPen(QPen(QColor(255, 255, 255, 20), 1.0));
        for (int tick = 0; tick <= 4; ++tick) {
            const double level = static_cast<double>(tick * 25);
            const double y = chart_rect.bottom() - ((level / 100.0) * chart_rect.height());
            painter.drawLine(QPointF(chart_rect.left(), y), QPointF(chart_rect.right(), y));
        }
        painter.setClipping(false);

        painter.setPen(QColor(QStringLiteral("#8892A2")));
        for (int tick = 0; tick <= 4; ++tick) {
            const int level = tick * 25;
            const double y = chart_rect.bottom() - ((static_cast<double>(level) / 100.0) * chart_rect.height());
            painter.drawText(QRectF(8.0, y - 10.0, kHistoryChartLeftPadding - 14.0, 20.0),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QStringLiteral("%1").arg(level));
        }

        const TimelineLayout timeline = BuildCompressedTimeline(history_, chart_rect);
        const QVector<HistorySeries> series_list = BuildSeries(chart_rect, timeline);

        if (series_list.isEmpty()) {
            painter.setPen(QColor(QStringLiteral("#E5E7EB")));
            painter.drawText(chart_rect, Qt::AlignCenter,
                             QString::fromUtf8(u8"За выбранные сутки пока нет точек."));
        } else {
            painter.setClipRect(chart_rect.adjusted(-6.0, -6.0, 6.0, 6.0));
            for (const auto& series : series_list) {
                const QColor color = ComponentColor(series.component_key);
                QPen line_pen(color, 2.25, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
                painter.setPen(line_pen);
                painter.setBrush(Qt::NoBrush);

                int valid_points = 0;
                for (const int level : series.levels) {
                    if (!IsDisconnectedLevel(level)) {
                        ++valid_points;
                    }
                }
                const bool draw_all_points = valid_points <= 24;

                int index = 0;
                while (index < series.points.size()) {
                    while (index < series.points.size() && IsDisconnectedLevel(series.levels[index])) {
                        ++index;
                    }
                    if (index >= series.points.size()) {
                        break;
                    }

                    const int segment_start = index;
                    while (index + 1 < series.points.size() && !IsSeriesBreak(series, index, index + 1)) {
                        ++index;
                    }
                    const int segment_end = index;

                    if (segment_start == segment_end) {
                        painter.setBrush(color);
                        painter.drawEllipse(series.points[segment_start], 4.0, 4.0);
                        ++index;
                        continue;
                    }

                    for (int point_index = segment_start + 1; point_index <= segment_end; ++point_index) {
                        painter.drawLine(series.points[point_index - 1], series.points[point_index]);
                    }

                    painter.setBrush(color);
                    if (draw_all_points) {
                        for (int point_index = segment_start; point_index <= segment_end; ++point_index) {
                            painter.drawEllipse(series.points[point_index], 3.2, 3.2);
                        }
                    } else {
                        painter.drawEllipse(series.points[segment_end], 3.2, 3.2);
                    }

                    ++index;
                }
            }
            painter.setClipping(false);
        }

        if (hover_active_ && !series_list.isEmpty()) {
            const qreal hover_x = std::clamp(hover_x_, chart_rect.left(), chart_rect.right());
            const auto hover_values = BuildHoverValues(series_list, hover_x);
            if (!hover_values.isEmpty()) {
                painter.setPen(QPen(QColor(255, 255, 255, 120), 1.0, Qt::DashLine));
                painter.drawLine(QPointF(hover_x, chart_rect.top()), QPointF(hover_x, chart_rect.bottom()));

                for (const auto& hover_value : hover_values) {
                    const QColor color = ComponentColor(hover_value.component_key);
                    painter.setPen(QPen(QColor(QStringLiteral("#1F2329")), 2.0));
                    painter.setBrush(color);
                    painter.drawEllipse(hover_value.point, 4.4, 4.4);
                }

                const qint64 hover_timestamp = InterpolateTimestampAtX(timeline, hover_x);
                DrawHoverOverlay(&painter, chart_rect, hover_timestamp, hover_values);
            }
        }

        painter.setPen(QColor(QStringLiteral("#8892A2")));
        const qint64 axis_start_timestamp =
            timeline.timestamps_ms.isEmpty() ? window_start_ms_ : timeline.timestamps_ms.front();
        const qint64 axis_end_timestamp =
            timeline.timestamps_ms.isEmpty() ? std::max(window_start_ms_ + 1, window_end_ms_ - 1)
                                             : timeline.timestamps_ms.back();
        const qint64 axis_middle_timestamp = timeline.timestamps_ms.isEmpty()
                                                 ? axis_start_timestamp + ((axis_end_timestamp - axis_start_timestamp) / 2)
                                                 : InterpolateTimestampAtX(timeline, chart_rect.center().x());
        painter.drawText(QRectF(chart_rect.left(), chart_rect.bottom() + 8.0, 120.0, 18.0),
                         Qt::AlignLeft,
                         FormatAxisLabel(axis_start_timestamp));
        painter.drawText(QRectF(chart_rect.center().x() - 80.0, chart_rect.bottom() + 8.0, 160.0, 18.0),
                         Qt::AlignHCenter,
                         FormatAxisLabel(axis_middle_timestamp));
        painter.drawText(QRectF(chart_rect.right() - 120.0, chart_rect.bottom() + 8.0, 120.0, 18.0),
                         Qt::AlignRight,
                         FormatAxisLabel(axis_end_timestamp));
    }

   private:
    QRectF ChartRect() const {
        return rect().adjusted(kHistoryChartLeftPadding, kHistoryChartTopPadding,
                               -kHistoryChartRightPadding, -kHistoryChartBottomPadding);
    }

    QVector<HistorySeries> BuildSeries(const QRectF& chart_rect, const TimelineLayout& timeline) const {
        QVector<HistorySeries> series_list;
        if (history_.samples.isEmpty() || timeline.x_positions.size() != history_.samples.size()) {
            return series_list;
        }

        for (const auto& component_key : VisibleComponents(history_)) {
            HistorySeries series;
            series.component_key = component_key;
            for (int sample_index = 0; sample_index < history_.samples.size(); ++sample_index) {
                const auto& sample = history_.samples[sample_index];
                const auto level_it = sample.component_levels.find(component_key);
                const int level = level_it == sample.component_levels.end() ? -1 : level_it.value();

                const double x = timeline.x_positions[sample_index];
                const double y =
                    chart_rect.bottom() -
                    ((static_cast<double>(std::max(level, 0)) / 100.0) * chart_rect.height());
                series.points.push_back(QPointF(x, y));
                series.timestamps_ms.push_back(sample.timestamp_ms);
                series.levels.push_back(level);
            }

            if (!series.points.isEmpty()) {
                series_list.push_back(std::move(series));
            }
        }

        return series_list;
    }

    QVector<HoverValue> BuildHoverValues(const QVector<HistorySeries>& series_list, qreal hover_x) const {
        QVector<HoverValue> values;
        for (const auto& series : series_list) {
            if (series.points.isEmpty()) {
                continue;
            }

            int index = 0;
            while (index < series.points.size()) {
                while (index < series.points.size() && IsDisconnectedLevel(series.levels[index])) {
                    ++index;
                }
                if (index >= series.points.size()) {
                    break;
                }

                const int segment_start = index;
                while (index + 1 < series.points.size() && !IsSeriesBreak(series, index, index + 1)) {
                    ++index;
                }
                const int segment_end = index;

                HoverValue value;
                value.component_key = series.component_key;

                if (segment_start == segment_end) {
                    if (std::abs(series.points[segment_start].x() - hover_x) <= 8.0) {
                        value.point = series.points[segment_start];
                        value.level = static_cast<double>(series.levels[segment_start]);
                        values.push_back(value);
                        break;
                    }
                    ++index;
                    continue;
                }

                if (hover_x < series.points[segment_start].x() || hover_x > series.points[segment_end].x()) {
                    ++index;
                    continue;
                }

                for (int point_index = segment_start; point_index + 1 <= segment_end; ++point_index) {
                    const QPointF left_point = series.points[point_index];
                    const QPointF right_point = series.points[point_index + 1];
                    if (hover_x < left_point.x() || hover_x > right_point.x()) {
                        continue;
                    }

                    const qreal segment_progress =
                        right_point.x() == left_point.x()
                            ? 0.0
                            : (hover_x - left_point.x()) / (right_point.x() - left_point.x());
                    value.point = QPointF(left_point.x() + ((right_point.x() - left_point.x()) * segment_progress),
                                          left_point.y() + ((right_point.y() - left_point.y()) * segment_progress));
                    value.level = static_cast<double>(series.levels[point_index]) +
                                  ((static_cast<double>(series.levels[point_index + 1]) -
                                    static_cast<double>(series.levels[point_index])) *
                                   static_cast<double>(segment_progress));
                    values.push_back(value);
                    break;
                }

                if (!values.isEmpty() && values.back().component_key == series.component_key) {
                    break;
                }

                ++index;
            }
        }

        std::sort(values.begin(), values.end(), [](const HoverValue& left, const HoverValue& right) {
            return ComponentOrder(left.component_key) < ComponentOrder(right.component_key);
        });
        return values;
    }

    void DrawHoverOverlay(QPainter* painter,
                          const QRectF& chart_rect,
                          qint64 hover_timestamp,
                          const QVector<HoverValue>& hover_values) const {
        if (painter == nullptr || hover_values.isEmpty()) {
            return;
        }

        painter->save();

        const QString timestamp_text = FormatFullTimestamp(hover_timestamp);
        const QFontMetrics metrics(painter->font());
        int overlay_width = metrics.horizontalAdvance(timestamp_text) + 24;
        int overlay_height = 32;

        QStringList value_lines;
        value_lines.reserve(hover_values.size());
        for (const auto& hover_value : hover_values) {
            const QString line = QStringLiteral("%1: %2%")
                                     .arg(ComponentLabel(hover_value.component_key))
                                     .arg(qRound(hover_value.level));
            value_lines.push_back(line);
            overlay_width = std::max(overlay_width, metrics.horizontalAdvance(line) + 34);
            overlay_height += 22;
        }

        QRectF overlay_rect(chart_rect.right() - overlay_width - 10.0, chart_rect.top() + 10.0,
                            overlay_width, overlay_height);
        painter->setPen(QPen(QColor(255, 255, 255, 38), 1.0));
        painter->setBrush(QColor(21, 24, 30, 224));
        painter->drawRoundedRect(overlay_rect, 12.0, 12.0);

        painter->setPen(QColor(QStringLiteral("#F8FAFC")));
        painter->drawText(QRectF(overlay_rect.left() + 12.0, overlay_rect.top() + 8.0,
                                 overlay_rect.width() - 24.0, 18.0),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          timestamp_text);

        qreal text_y = overlay_rect.top() + 34.0;
        for (int index = 0; index < hover_values.size(); ++index) {
            const auto& hover_value = hover_values[index];
            const QColor color = ComponentColor(hover_value.component_key);
            painter->setBrush(color);
            painter->setPen(Qt::NoPen);
            painter->drawEllipse(QRectF(overlay_rect.left() + 12.0, text_y + 4.0, 8.0, 8.0));

            painter->setPen(QColor(QStringLiteral("#E7ECF6")));
            painter->drawText(QRectF(overlay_rect.left() + 26.0, text_y - 2.0,
                                     overlay_rect.width() - 38.0, 18.0),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              value_lines[index]);
            text_y += 22.0;
        }

        painter->restore();
    }

    BatteryHistoryData history_;
    qint64 window_start_ms_ = QDateTime(QDate::currentDate(), QTime(0, 0)).toMSecsSinceEpoch();
    qint64 window_end_ms_ = window_start_ms_ + kDayRangeMs;
    bool hover_active_ = false;
    qreal hover_x_ = 0.0;
};

BatteryHistoryDialog::BatteryHistoryDialog(BatteryHistoryData history, QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("historyDialog"));
    setWindowTitle(QString::fromUtf8(u8"График разрядки"));
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(700, 460);
    setMinimumSize(620, 380);

    setStyleSheet(R"(
QDialog#historyDialog {
    background: #3B3E44;
    color: #F7F7F7;
    border: 1px solid rgba(255, 255, 255, 0.09);
    border-radius: 14px;
}
QLabel#historyTitle {
    color: #F8FAFC;
    font-size: 16px;
    font-weight: 700;
}
QLabel#historySubtitle {
    color: #D1D5DB;
    font-size: 11px;
}
QLabel#historyStats {
    color: #AEB9C8;
    font-size: 11px;
}
QFrame#historySummaryCard {
    background: #2A2F37;
    border: 1px solid rgba(255, 255, 255, 0.10);
    border-radius: 12px;
}
QLabel#historySummaryTitle {
    color: #AEB9C8;
    font-size: 11px;
    font-weight: 600;
}
QLabel#historySummaryValue {
    color: #F8FAFC;
    font-size: 20px;
    font-weight: 800;
}
QLabel#historySummaryNote {
    color: #D1D5DB;
    font-size: 11px;
}
QLabel#historyDayLabel {
    color: #F8FAFC;
    font-size: 12px;
    font-weight: 700;
}
QLabel#historyHint {
    color: #8892A2;
    font-size: 11px;
}
QLabel#historyLegendChip {
    background: #2A2F37;
    border: 1px solid rgba(255, 255, 255, 0.14);
    border-radius: 10px;
    color: #E7ECF6;
    padding: 4px 10px;
    font-size: 11px;
    font-weight: 600;
}
QPushButton#historyLegendButton {
    background: #2A2F37;
    border: 1px solid rgba(255, 255, 255, 0.14);
    border-radius: 10px;
    color: #E7ECF6;
    padding: 4px 10px;
    font-size: 11px;
    font-weight: 600;
    text-align: left;
}
QPushButton#historyLegendButton:checked {
    background: rgba(255, 255, 255, 0.04);
}
QPushButton#historyLegendButton:!checked {
    background: rgba(255, 255, 255, 0.01);
    color: rgba(231, 236, 246, 0.48);
    border-color: rgba(255, 255, 255, 0.08);
}
QPushButton#historyCloseButton,
QPushButton#historyNavButton {
    background: #44484F;
    color: #F8FAFC;
    border: 1px solid rgba(255, 255, 255, 0.14);
    border-radius: 10px;
    padding: 6px 12px;
    font-size: 11px;
    font-weight: 600;
}
QPushButton#historyCloseButton:hover,
QPushButton#historyNavButton:hover {
    background: #50555E;
}
QPushButton#historyNavButton:disabled {
    color: rgba(248, 250, 252, 0.45);
    background: #3F444C;
}
)");

    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(18, 18, 18, 16);
    root_layout->setSpacing(10);

    title_label_ = new QLabel(this);
    title_label_->setObjectName(QStringLiteral("historyTitle"));

    subtitle_label_ = new QLabel(this);
    subtitle_label_->setObjectName(QStringLiteral("historySubtitle"));
    subtitle_label_->setWordWrap(true);

    stats_label_ = new QLabel(this);
    stats_label_->setObjectName(QStringLiteral("historyStats"));
    stats_label_->setWordWrap(true);

    summary_container_ = new QFrame(this);
    auto* summary_layout = new QHBoxLayout(summary_container_);
    summary_layout->setContentsMargins(0, 0, 0, 0);
    summary_layout->setSpacing(10);
    for (int index = 0; index < static_cast<int>(summary_cards_.size()); ++index) {
        summary_cards_[index] = CreateSummaryCard(&summary_title_labels_[index],
                                                  &summary_value_labels_[index],
                                                  &summary_note_labels_[index],
                                                  summary_container_);
        summary_layout->addWidget(summary_cards_[index]);
    }

    auto* day_row = new QHBoxLayout();
    day_row->setContentsMargins(0, 0, 0, 0);
    day_row->setSpacing(8);

    previous_day_button_ = new QPushButton(QString::fromUtf8(u8"← День"), this);
    previous_day_button_->setObjectName(QStringLiteral("historyNavButton"));
    connect(previous_day_button_, &QPushButton::clicked, this, [this]() { ShiftSelectedDay(-1); });

    day_label_ = new QLabel(this);
    day_label_->setObjectName(QStringLiteral("historyDayLabel"));

    next_day_button_ = new QPushButton(QString::fromUtf8(u8"День →"), this);
    next_day_button_->setObjectName(QStringLiteral("historyNavButton"));
    connect(next_day_button_, &QPushButton::clicked, this, [this]() { ShiftSelectedDay(1); });

    day_row->addWidget(previous_day_button_, 0, Qt::AlignLeft);
    day_row->addWidget(day_label_, 0, Qt::AlignLeft);
    day_row->addStretch(1);
    day_row->addWidget(next_day_button_, 0, Qt::AlignRight);

    chart_widget_ = new BatteryHistoryChartWidget(this);

    legend_container_ = new QWidget(this);
    legend_layout_ = new QHBoxLayout(legend_container_);
    legend_layout_->setContentsMargins(0, 0, 0, 0);
    legend_layout_->setSpacing(8);

    hint_label_ = new QLabel(this);
    hint_label_->setObjectName(QStringLiteral("historyHint"));
    hint_label_->setWordWrap(true);

    auto* button_row = new QHBoxLayout();
    button_row->setContentsMargins(0, 0, 0, 0);
    button_row->addStretch(1);

    auto* close_button = new QPushButton(QString::fromUtf8(u8"Закрыть"), this);
    close_button->setObjectName(QStringLiteral("historyCloseButton"));
    connect(close_button, &QPushButton::clicked, this, &QDialog::close);
    button_row->addWidget(close_button);

    root_layout->addWidget(title_label_);
    root_layout->addWidget(subtitle_label_);
    root_layout->addWidget(summary_container_);
    root_layout->addWidget(stats_label_);
    root_layout->addLayout(day_row);
    root_layout->addWidget(chart_widget_, 1);
    root_layout->addWidget(legend_container_);
    root_layout->addWidget(hint_label_);
    root_layout->addLayout(button_row);

    SetHistory(std::move(history));
}

void BatteryHistoryDialog::SetHistory(BatteryHistoryData history) {
    history_ = std::move(history);

    const auto days = AvailableDays(history_);
    if (days.isEmpty()) {
        selected_day_ = DefaultSelectedDay(history_);
    } else if (!selected_day_.isValid() || !days.contains(selected_day_)) {
        selected_day_ = days.back();
    }

    RefreshUi();
}

void BatteryHistoryDialog::SetRuntimeDeadline(std::optional<qint64> runtime_deadline_ms) {
    runtime_deadline_ms_ = runtime_deadline_ms;
    RefreshUi();
}

void BatteryHistoryDialog::SetComponentRuntimeDeadlines(QHash<QString, qint64> component_runtime_deadlines_ms) {
    component_runtime_deadlines_ms_ = std::move(component_runtime_deadlines_ms);
    RefreshUi();
}

void BatteryHistoryDialog::RefreshUi() {
    const BatteryHistoryData day_history = FilterHistoryForDay(history_, selected_day_);
    const BatteryHistoryData visible_history = FilterHiddenComponents(day_history, hidden_components_);
    const auto available_days = AvailableDays(history_);
    const int day_index = DayIndex(available_days, selected_day_);
    const BatteryRuntimeForecast runtime_forecast = EstimateBatteryRuntimeForecast(history_);
    const QStringList component_keys = VisibleComponents(history_);
    QStringList summary_component_keys;
    const auto append_if_present = [&](const QString& component_key) {
        if (component_keys.contains(component_key) && !summary_component_keys.contains(component_key)) {
            summary_component_keys.push_back(component_key);
        }
    };
    append_if_present(QStringLiteral("left"));
    append_if_present(QStringLiteral("case"));
    append_if_present(QStringLiteral("right"));
    if (summary_component_keys.isEmpty()) {
        append_if_present(QStringLiteral("main"));
    }
    if (summary_component_keys.isEmpty()) {
        summary_component_keys = component_keys;
    }

    title_label_->setText(BuildDialogTitle(history_));
    subtitle_label_->setText(BuildCompactHistorySubtitle(history_, day_history, selected_day_));
    bool has_visible_summary_card = false;
    for (int index = 0; index < static_cast<int>(summary_cards_.size()); ++index) {
        if (index >= summary_component_keys.size()) {
            summary_cards_[index]->setVisible(false);
            continue;
        }

        const QString component_key = summary_component_keys[index];
        const SummaryCardText component_card = BuildComponentCard(component_key,
                                                                  history_,
                                                                  component_runtime_deadlines_ms_,
                                                                  runtime_deadline_ms_,
                                                                  runtime_forecast);
        summary_title_labels_[index]->setText(ComponentLabel(component_key));
        summary_value_labels_[index]->setText(component_card.value);
        summary_value_labels_[index]->setVisible(!component_card.value.trimmed().isEmpty());
        summary_note_labels_[index]->setText(component_card.note);
        summary_note_labels_[index]->setVisible(!component_card.note.trimmed().isEmpty());
        summary_cards_[index]->setVisible(true);
        has_visible_summary_card = true;
    }

    summary_container_->setVisible(has_visible_summary_card);
    const QString compact_stats = BuildCompactStatsLine(history_);
    stats_label_->setText(compact_stats);
    stats_label_->setVisible(!compact_stats.isEmpty());
    day_label_->setText(FormatDayLabel(selected_day_));
    hint_label_->setText(BuildHintText(history_));
    chart_widget_->SetHistory(visible_history, DayWindowStartMs(selected_day_), DayWindowEndMs(selected_day_));
    previous_day_button_->setEnabled(day_index > 0);
    next_day_button_->setEnabled(day_index >= 0 && day_index + 1 < available_days.size());
    RebuildLegend(day_history);
}

void BatteryHistoryDialog::RebuildLegend(const BatteryHistoryData& visible_history) {
    if (legend_layout_ == nullptr) {
        return;
    }

    QLayoutItem* item = nullptr;
    while ((item = legend_layout_->takeAt(0)) != nullptr) {
        if (item->widget() != nullptr) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    const auto visible_components = VisibleComponents(visible_history);
    if (visible_components.isEmpty()) {
        auto* empty_label = new QLabel(QString::fromUtf8(u8"За выбранные сутки сохранённых компонентов пока нет."),
                                       legend_container_);
        empty_label->setObjectName(QStringLiteral("historyHint"));
        legend_layout_->addWidget(empty_label);
        legend_layout_->addStretch(1);
        return;
    }

    for (const auto& component_key : visible_components) {
        auto* chip = new QPushButton(ComponentLabel(component_key), legend_container_);
        chip->setObjectName(QStringLiteral("historyLegendButton"));
        chip->setCheckable(true);
        chip->setChecked(!hidden_components_.contains(component_key));
        const QColor color = ComponentColor(component_key);
        chip->setStyleSheet(QStringLiteral(
                                "QPushButton#historyLegendButton { background: #2A2F37; "
                                "border: 1px solid %1; color: %2; border-radius: 10px; padding: 4px 10px; "
                                "font-size: 11px; font-weight: 600; } "
                                "QPushButton#historyLegendButton:!checked { "
                                "background: rgba(255,255,255,0.01); color: rgba(231,236,246,0.48); "
                                "border: 1px solid rgba(255,255,255,0.08); }")
                                .arg(color.name(), color.name()));
        connect(chip, &QPushButton::clicked, this, [this, component_key]() { ToggleComponent(component_key); });
        legend_layout_->addWidget(chip, 0, Qt::AlignLeft);
    }
    legend_layout_->addStretch(1);
}

void BatteryHistoryDialog::ShiftSelectedDay(int offset) {
    const auto available_days = AvailableDays(history_);
    const int current_index = DayIndex(available_days, selected_day_);
    if (current_index < 0) {
        return;
    }

    const int max_index = static_cast<int>(available_days.size()) - 1;
    const int next_index = std::clamp(current_index + offset, 0, max_index);
    if (next_index == current_index) {
        return;
    }

    selected_day_ = available_days[next_index];
    RefreshUi();
}

void BatteryHistoryDialog::ToggleComponent(const QString& component_key) {
    if (hidden_components_.contains(component_key)) {
        hidden_components_.remove(component_key);
    } else {
        hidden_components_.insert(component_key);
    }
    RefreshUi();
}

}  // namespace battery_monitor
