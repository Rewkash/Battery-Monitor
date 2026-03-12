#pragma once

#include <optional>

#include <QDialog>

#include "ui/BatteryHistoryStore.h"

class QLabel;
class QComboBox;
class QFrame;
class QTimer;
class QToolButton;
class QTextBrowser;
class QWidget;

namespace battery_monitor {

class BatteryStatsDialog : public QDialog {
   public:
    explicit BatteryStatsDialog(BatteryHistoryData history, QWidget* parent = nullptr);

    void SetHistory(BatteryHistoryData history);

   private:
    void RefreshUi();
    void UpdateCountdownLabels();

    BatteryHistoryData history_;
    QString selected_scenario_mode_ = QStringLiteral("current");
    QString selected_scenario_submode_;
    bool updating_scenario_controls_ = false;
    std::optional<qint64> left_countdown_deadline_ms_;
    std::optional<qint64> right_countdown_deadline_ms_;
    QString left_countdown_state_key_;
    QString right_countdown_state_key_;

    QLabel* title_label_ = nullptr;
    QLabel* history_meta_label_ = nullptr;
    QLabel* mode_label_ = nullptr;
    QLabel* scenario_label_ = nullptr;
    QWidget* scenario_controls_widget_ = nullptr;
    QComboBox* scenario_mode_combo_ = nullptr;
    QComboBox* scenario_submode_combo_ = nullptr;

    QFrame* left_card_ = nullptr;
    QLabel* left_title_label_ = nullptr;
    QLabel* left_percent_label_ = nullptr;
    QLabel* left_eta_label_ = nullptr;
    QLabel* left_confidence_badge_ = nullptr;
    QLabel* left_note_label_ = nullptr;

    QFrame* right_card_ = nullptr;
    QLabel* right_title_label_ = nullptr;
    QLabel* right_percent_label_ = nullptr;
    QLabel* right_eta_label_ = nullptr;
    QLabel* right_confidence_badge_ = nullptr;
    QLabel* right_note_label_ = nullptr;

    QLabel* imbalance_label_ = nullptr;
    QLabel* imbalance_details_label_ = nullptr;
    QLabel* primary_insight_label_ = nullptr;
    QLabel* secondary_insight_label_ = nullptr;

    QToolButton* details_toggle_button_ = nullptr;
    QWidget* details_container_ = nullptr;
    QTextBrowser* details_view_ = nullptr;
    QTimer* countdown_timer_ = nullptr;
};

}  // namespace battery_monitor
