#pragma once

#include <QDialog>
#include <QDate>
#include <QSet>

#include "ui/BatteryHistoryStore.h"

class QHBoxLayout;
class QLabel;
class QPushButton;
class QWidget;

namespace battery_monitor {

class BatteryHistoryChartWidget;

class BatteryHistoryDialog : public QDialog {
   public:
    explicit BatteryHistoryDialog(BatteryHistoryData history, QWidget* parent = nullptr);

    void SetHistory(BatteryHistoryData history);

   private:
    void RefreshUi();
    void RebuildLegend(const BatteryHistoryData& visible_history);
    void ShiftSelectedDay(int offset);
    void ToggleComponent(const QString& component_key);

    BatteryHistoryData history_;
    QDate selected_day_;
    QSet<QString> hidden_components_;
    QLabel* title_label_ = nullptr;
    QLabel* subtitle_label_ = nullptr;
    QLabel* stats_label_ = nullptr;
    QLabel* day_label_ = nullptr;
    QLabel* hint_label_ = nullptr;
    QWidget* legend_container_ = nullptr;
    QHBoxLayout* legend_layout_ = nullptr;
    QPushButton* previous_day_button_ = nullptr;
    QPushButton* next_day_button_ = nullptr;
    BatteryHistoryChartWidget* chart_widget_ = nullptr;
};

}  // namespace battery_monitor
