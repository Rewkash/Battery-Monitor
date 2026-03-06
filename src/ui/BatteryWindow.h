#pragma once

#include <exception>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QDateTime>
#include <QEvent>
#include <QPointer>
#include <QWidget>

#include "core/BatteryTypes.h"
#include "core/IBluetoothBatteryProvider.h"
#include "ui/BatteryHistoryStore.h"

class QAction;
class QLabel;
class QMenu;
class QPaintEvent;
class QPropertyAnimation;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QSystemTrayIcon;
class QTimer;
class QToolButton;
class QVBoxLayout;
class QWidget;
class QCloseEvent;

namespace battery_monitor {

class BatteryHistoryDialog;

class BatteryWindow : public QWidget {
   public:
    explicit BatteryWindow(std::unique_ptr<IBluetoothBatteryProvider> provider, QWidget* parent = nullptr);
    ~BatteryWindow() override;
    void Launch();

   protected:
    void closeEvent(QCloseEvent* event) override;
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

   private:
    void RefreshBatteryData();
    void PopulateDeviceCards(const std::vector<DeviceBatteryInfo>& devices);
    void ClearDeviceCards();
    void InitializeTray();
    void ShowWindowFromTray();
    void HideWindowToTray();
    void QuitFromTray();
    void ResetHiddenDevices();
    void UpdateToggleActionText();
    void UpdateTrayTooltip(const std::vector<DeviceBatteryInfo>& devices);
    void NotifyLowBatteryIfNeeded(const std::vector<DeviceBatteryInfo>& devices);
    void RecordBatteryHistory(const std::vector<DeviceBatteryInfo>& devices);
    void ShowBatteryHistory(const std::string& device_id, const std::string& fallback_name);
    void AdjustWindowHeightForRows(int visible_rows);
    void ApplyRefreshIntervalSeconds(int seconds, bool announce_status);
    void ApplyLowBatteryThresholdPercent(int percent, bool announce_status);
    void ApplyLowBatteryRepeatMinutes(int minutes, bool announce_status);
    void ConfigureRefreshInterval();
    void UpdateRefreshSettingsTooltip();
    void SetDeviceDragActive(bool active);
    static std::string FormatError(const std::exception& ex);

    std::unique_ptr<IBluetoothBatteryProvider> provider_;
    QWidget* cards_container_ = nullptr;
    QVBoxLayout* cards_layout_ = nullptr;
    QScrollArea* scroll_area_ = nullptr;
    QWidget* settings_panel_ = nullptr;
    QPropertyAnimation* settings_panel_animation_ = nullptr;
    QSpinBox* refresh_interval_spinbox_ = nullptr;
    QSpinBox* low_battery_threshold_spinbox_ = nullptr;
    QSpinBox* low_battery_repeat_spinbox_ = nullptr;
    QPushButton* refresh_button_ = nullptr;
    QPushButton* show_all_button_ = nullptr;
    QToolButton* settings_button_ = nullptr;
    QLabel* summary_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QSystemTrayIcon* tray_icon_ = nullptr;
    QMenu* tray_menu_ = nullptr;
    QAction* toggle_window_action_ = nullptr;
    QAction* refresh_action_ = nullptr;
    QAction* reset_hidden_action_ = nullptr;
    QAction* quit_action_ = nullptr;
    QTimer* refresh_timer_ = nullptr;
    std::unordered_set<std::string> hidden_device_ids_;
    std::unordered_map<std::string, QDateTime> last_live_update_;
    std::vector<std::string> connected_device_order_;
    std::vector<std::string> disconnected_device_order_;
    std::vector<DeviceBatteryInfo> last_devices_snapshot_;
    std::unordered_map<std::string, std::uint8_t> last_live_component_levels_;
    std::unordered_map<std::string, std::int64_t> last_low_battery_alert_ms_;
    std::unordered_map<std::string, QPointer<BatteryHistoryDialog>> history_dialogs_;
    std::thread refresh_worker_;
    std::atomic<bool> refresh_in_progress_{false};
    bool refresh_pending_ = false;
    bool drag_in_progress_ = false;
    bool settings_panel_expanded_ = false;
    int refresh_interval_ms_ = 15000;
    int low_battery_threshold_percent_ = 10;
    int low_battery_repeat_minutes_ = 10;
    bool quitting_ = false;
    BatteryHistoryStore history_store_;
};

}  // namespace battery_monitor
