#pragma once

#include <QDialog>

#include "ui/BatteryHistoryStore.h"

class QHBoxLayout;
class QLabel;
class QWidget;

namespace battery_monitor {

class BatteryHistoryChartWidget;

class BatteryHistoryDialog : public QDialog {
   public:
    explicit BatteryHistoryDialog(BatteryHistoryData history, QWidget* parent = nullptr);

    void SetHistory(BatteryHistoryData history);

   private:
    void RefreshUi();
    void RebuildLegend();

    BatteryHistoryData history_;
    QLabel* title_label_ = nullptr;
    QLabel* subtitle_label_ = nullptr;
    QLabel* hint_label_ = nullptr;
    QWidget* legend_container_ = nullptr;
    QHBoxLayout* legend_layout_ = nullptr;
    BatteryHistoryChartWidget* chart_widget_ = nullptr;
};

}  // namespace battery_monitor
