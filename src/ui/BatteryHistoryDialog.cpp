#include "ui/BatteryHistoryDialog.h"

#include <algorithm>
#include <array>

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
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
};

constexpr int kHistoryChartLeftPadding = 48;
constexpr int kHistoryChartTopPadding = 16;
constexpr int kHistoryChartRightPadding = 18;
constexpr int kHistoryChartBottomPadding = 34;

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

QString FormatAxisLabel(qint64 timestamp_ms, qint64 range_ms) {
    const QDateTime timestamp = QDateTime::fromMSecsSinceEpoch(timestamp_ms);
    const QLocale locale = QLocale::system();
    if (range_ms < 24LL * 60LL * 60LL * 1000LL) {
        return locale.toString(timestamp.time(), QStringLiteral("HH:mm"));
    }
    if (range_ms < 7LL * 24LL * 60LL * 60LL * 1000LL) {
        return locale.toString(timestamp, QStringLiteral("dd MMM HH:mm"));
    }
    return locale.toString(timestamp.date(), QStringLiteral("dd MMM"));
}

QString FormatFullTimestamp(qint64 timestamp_ms) {
    return QLocale::system().toString(QDateTime::fromMSecsSinceEpoch(timestamp_ms),
                                      QStringLiteral("dd MMM yyyy HH:mm"));
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

QString BuildDialogSubtitle(const BatteryHistoryData& history) {
    if (history.samples.isEmpty()) {
        return QString::fromUtf8(u8"История появится после нескольких живых обновлений.");
    }

    const auto first_timestamp = history.samples.front().timestamp_ms;
    const auto last_timestamp = history.samples.back().timestamp_ms;
    return QString::fromUtf8(u8"Точек: %1  ·  %2 - %3")
        .arg(history.samples.size())
        .arg(FormatFullTimestamp(first_timestamp))
        .arg(FormatFullTimestamp(last_timestamp));
}

QString BuildHintText(const BatteryHistoryData& history) {
    if (history.samples.size() < 2) {
        return QString::fromUtf8(
            u8"Для графика нужна хотя бы пара измерений. Сохраняются только live-значения без cached и offline.");
    }
    return QString::fromUtf8(
        u8"График строится только по живым значениям. Одинаковые точки пишутся не чаще одного раза в 10 минут.");
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

}  // namespace

class BatteryHistoryChartWidget final : public QWidget {
   public:
    explicit BatteryHistoryChartWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumHeight(220);
    }

    void SetHistory(BatteryHistoryData history) {
        history_ = std::move(history);
        update();
    }

   protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), Qt::transparent);

        QRectF frame_rect = rect().adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(QPen(QColor(255, 255, 255, 23), 1.0));
        painter.setBrush(QColor(QStringLiteral("#2A2F37")));
        painter.drawRoundedRect(frame_rect, 14.0, 14.0);

        QRectF chart_rect = frame_rect.adjusted(kHistoryChartLeftPadding, kHistoryChartTopPadding,
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

        if (history_.samples.isEmpty()) {
            painter.setPen(QColor(QStringLiteral("#E5E7EB")));
            painter.drawText(chart_rect, Qt::AlignCenter,
                             QString::fromUtf8(u8"Пока нет истории.\nОставьте окно работать несколько обновлений."));
            return;
        }

        const qint64 min_timestamp = history_.samples.front().timestamp_ms;
        qint64 max_timestamp = history_.samples.back().timestamp_ms;
        if (max_timestamp <= min_timestamp) {
            max_timestamp = min_timestamp + 1;
        }
        const qint64 range_ms = std::max<qint64>(1, max_timestamp - min_timestamp);

        QVector<HistorySeries> series_list;
        for (const auto& component_key : VisibleComponents(history_)) {
            HistorySeries series;
            series.component_key = component_key;
            for (const auto& sample : history_.samples) {
                const auto level_it = sample.component_levels.find(component_key);
                if (level_it == sample.component_levels.end()) {
                    continue;
                }

                const double progress = static_cast<double>(sample.timestamp_ms - min_timestamp) /
                                        static_cast<double>(range_ms);
                const double x = chart_rect.left() + (progress * chart_rect.width());
                const double y = chart_rect.bottom() -
                                 ((static_cast<double>(level_it.value()) / 100.0) * chart_rect.height());
                series.points.push_back(QPointF(x, y));
            }

            if (!series.points.isEmpty()) {
                series_list.push_back(std::move(series));
            }
        }

        if (series_list.isEmpty()) {
            painter.setPen(QColor(QStringLiteral("#E5E7EB")));
            painter.drawText(chart_rect, Qt::AlignCenter,
                             QString::fromUtf8(u8"В истории нет пригодных точек для построения."));
            return;
        }

        painter.setClipRect(chart_rect.adjusted(-6.0, -6.0, 6.0, 6.0));
        for (const auto& series : series_list) {
            const QColor color = ComponentColor(series.component_key);
            QPen line_pen(color, 2.25, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            painter.setPen(line_pen);
            painter.setBrush(Qt::NoBrush);

            if (series.points.size() == 1) {
                painter.setBrush(color);
                painter.drawEllipse(series.points.front(), 4.0, 4.0);
                continue;
            }

            QPainterPath path(series.points.front());
            for (int index = 1; index < series.points.size(); ++index) {
                path.lineTo(series.points[index]);
            }
            painter.drawPath(path);

            painter.setBrush(color);
            const bool draw_all_points = series.points.size() <= 24;
            for (int index = 0; index < series.points.size(); ++index) {
                if (!draw_all_points && index + 1 < series.points.size()) {
                    continue;
                }
                painter.drawEllipse(series.points[index], 3.2, 3.2);
            }
        }
        painter.setClipping(false);

        painter.setPen(QColor(QStringLiteral("#8892A2")));
        const qint64 middle_timestamp = min_timestamp + (range_ms / 2);
        const QString start_text = FormatAxisLabel(min_timestamp, range_ms);
        const QString middle_text = FormatAxisLabel(middle_timestamp, range_ms);
        const QString end_text = FormatAxisLabel(max_timestamp, range_ms);

        painter.drawText(QRectF(chart_rect.left(), chart_rect.bottom() + 8.0, 120.0, 18.0), Qt::AlignLeft,
                         start_text);
        painter.drawText(QRectF(chart_rect.center().x() - 80.0, chart_rect.bottom() + 8.0, 160.0, 18.0),
                         Qt::AlignHCenter, middle_text);
        painter.drawText(QRectF(chart_rect.right() - 120.0, chart_rect.bottom() + 8.0, 120.0, 18.0), Qt::AlignRight,
                         end_text);
    }

   private:
    BatteryHistoryData history_;
};

BatteryHistoryDialog::BatteryHistoryDialog(BatteryHistoryData history, QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("historyDialog"));
    setWindowTitle(QString::fromUtf8(u8"График разрядки"));
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(620, 420);
    setMinimumSize(540, 360);

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
QPushButton#historyCloseButton {
    background: #44484F;
    color: #F8FAFC;
    border: 1px solid rgba(255, 255, 255, 0.14);
    border-radius: 10px;
    padding: 6px 12px;
    font-size: 11px;
    font-weight: 600;
}
QPushButton#historyCloseButton:hover {
    background: #50555E;
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
    root_layout->addWidget(chart_widget_, 1);
    root_layout->addWidget(legend_container_);
    root_layout->addWidget(hint_label_);
    root_layout->addLayout(button_row);

    SetHistory(std::move(history));
}

void BatteryHistoryDialog::SetHistory(BatteryHistoryData history) {
    history_ = std::move(history);
    RefreshUi();
}

void BatteryHistoryDialog::RefreshUi() {
    title_label_->setText(BuildDialogTitle(history_));
    subtitle_label_->setText(BuildDialogSubtitle(history_));
    hint_label_->setText(BuildHintText(history_));
    chart_widget_->SetHistory(history_);
    RebuildLegend();
}

void BatteryHistoryDialog::RebuildLegend() {
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

    const auto visible_components = VisibleComponents(history_);
    if (visible_components.isEmpty()) {
        auto* empty_label = new QLabel(QString::fromUtf8(u8"Компоненты появятся после первых сохранённых точек."),
                                       legend_container_);
        empty_label->setObjectName(QStringLiteral("historyHint"));
        legend_layout_->addWidget(empty_label);
        legend_layout_->addStretch(1);
        return;
    }

    for (const auto& component_key : visible_components) {
        auto* chip = new QLabel(ComponentLabel(component_key), legend_container_);
        chip->setObjectName(QStringLiteral("historyLegendChip"));
        const QColor color = ComponentColor(component_key);
        chip->setStyleSheet(QStringLiteral(
                                "QLabel#historyLegendChip { background: #2A2F37; "
                                "border: 1px solid %1; color: %2; border-radius: 10px; padding: 4px 10px; "
                                "font-size: 11px; font-weight: 600; }")
                                .arg(color.name(), color.name()));
        legend_layout_->addWidget(chip, 0, Qt::AlignLeft);
    }
    legend_layout_->addStretch(1);
}

}  // namespace battery_monitor
