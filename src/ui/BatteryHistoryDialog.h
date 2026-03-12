#pragma once

#include <array>

#include <QDate>
#include <QDialog>
#include <QHash>
#include <QSet>

#include "ui/BatteryHistoryStore.h"

class QHBoxLayout;
class QFrame;
class QLabel;
class QPushButton;
class QWidget;

namespace battery_monitor {

class BatteryHistoryChartWidget;

class BatteryHistoryDialog : public QDialog {
   public:
    explicit BatteryHistoryDialog(BatteryHistoryData history, QWidget* parent = nullptr);

    void SetHistory(BatteryHistoryData history);
    void SetRuntimeDeadline(std::optional<qint64> runtime_deadline_ms);
    void SetComponentRuntimeDeadlines(QHash<QString, qint64> component_runtime_deadlines_ms);

   private:
    void RefreshUi();
    void RebuildLegend(const BatteryHistoryData& visible_history);
    void ShiftSelectedDay(int offset);
    void ToggleComponent(const QString& component_key);

    BatteryHistoryData history_;
    QDate selected_day_;
    QSet<QString> hidden_components_;
    std::optional<qint64> runtime_deadline_ms_;
    QHash<QString, qint64> component_runtime_deadlines_ms_;
    QFrame* summary_container_ = nullptr;
    QLabel* title_label_ = nullptr;
    QLabel* subtitle_label_ = nullptr;
    QLabel* stats_label_ = nullptr;
    std::array<QFrame*, 3> summary_cards_{};
    std::array<QLabel*, 3> summary_title_labels_{};
    std::array<QLabel*, 3> summary_value_labels_{};
    std::array<QLabel*, 3> summary_note_labels_{};
    QLabel* day_label_ = nullptr;
    QLabel* hint_label_ = nullptr;
    QWidget* legend_container_ = nullptr;
    QHBoxLayout* legend_layout_ = nullptr;
    QPushButton* previous_day_button_ = nullptr;
    QPushButton* next_day_button_ = nullptr;
    BatteryHistoryChartWidget* chart_widget_ = nullptr;
};

}  // namespace battery_monitor
