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
#include "core/INoiseControlProvider.h"
#include "ui/BatteryHistoryStore.h"
#include "ui/BatteryNotificationController.h"
#include "ui/BatteryWindowSettings.h"

#ifdef _WIN32
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/base.h>
#endif

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
class BatteryStatsDialog;
class DeviceDiagnosticsDialog;
class UpdateDialog;
class UpdateService;
struct UpdateManifest;

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
     void RefreshBatteryData(bool include_disconnected = false,
                             bool preserve_disconnected_snapshot = true,
                             bool force_live_refresh = false);
     void RefreshBatteryDataFromUser();
    void RefreshBatteryDataForDevice(const std::string& device_id, bool force_live_refresh = false);
    void PopulateDeviceCards(const std::vector<DeviceBatteryInfo>& devices);
    void ClearDeviceCards();
     void InitializeTray();
     void CheckForApplicationUpdates(bool user_initiated);
     void ShowApplicationUpdate(const UpdateManifest& manifest);
     void PrepareForUpdateExit();
    void ShowWindowFromTray();
    void HideWindowToTray();
    void QuitFromTray();
    void ResetHiddenDevices();
    void UpdateToggleActionText();
    void UpdateTrayTooltip(const std::vector<DeviceBatteryInfo>& devices);
    void NotifyLowBatteryIfNeeded(const std::vector<DeviceBatteryInfo>& devices);
    void RecordBatteryHistory(const std::vector<DeviceBatteryInfo>& devices);
    void ShowBatteryHistory(const std::string& device_id, const std::string& fallback_name);
    void ShowBatteryStats(const std::string& device_id, const std::string& fallback_name);
    void ShowDeviceDiagnostics(const std::string& device_id);
    void AdjustWindowHeightForRows(int visible_rows);
    void ApplyRefreshIntervalSeconds(int seconds, bool announce_status);
    void ApplyLowBatteryThresholdPercent(int percent, bool announce_status);
    void ApplyLowBatteryRepeatMinutes(int minutes, bool announce_status);
    void ConfigureRefreshInterval();
    void UpdateRefreshSettingsTooltip();
    void UpdateRuntimeCountdownLabels();
    void SetDeviceDragActive(bool active);
    void StartBluetoothDeviceWatcher();
    void StopBluetoothDeviceWatcher();
    void ScheduleBluetoothDeviceRefresh(const std::string& changed_device_id, bool connected);
    bool ApplyBluetoothDeviceConnectionChange(const std::string& changed_device_id, bool connected);
    void ApplyNoiseControlMode(const std::string& device_id, NoiseControlMode mode);
    void ApplyNoiseSubmode(const std::string& device_id, NoiseControlMode mode, const std::string& submode_id);
    void ShowNoiseSubmodeMenu(QWidget* anchor,
                              const std::string& device_id,
                              NoiseControlMode mode,
                              const std::string& active_submode_id);
    static std::string FormatError(const std::exception& ex);

    std::unique_ptr<IBluetoothBatteryProvider> provider_;
    INoiseControlProvider* noise_control_provider_ = nullptr;
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
    QAction* autostart_action_ = nullptr;
    QAction* check_updates_action_ = nullptr;
    QAction* quit_action_ = nullptr;
    QTimer* refresh_timer_ = nullptr;
    QTimer* runtime_timer_ = nullptr;
    QTimer* bluetooth_refresh_debounce_timer_ = nullptr;
    QTimer* push_refresh_debounce_timer_ = nullptr;
#ifdef _WIN32
    winrt::Windows::Devices::Enumeration::DeviceWatcher bluetooth_classic_watcher_{nullptr};
    winrt::Windows::Devices::Enumeration::DeviceWatcher bluetooth_le_watcher_{nullptr};
    winrt::event_token bluetooth_classic_added_token_{};
    winrt::event_token bluetooth_classic_updated_token_{};
    winrt::event_token bluetooth_classic_removed_token_{};
    winrt::event_token bluetooth_le_added_token_{};
    winrt::event_token bluetooth_le_updated_token_{};
    winrt::event_token bluetooth_le_removed_token_{};
#endif
    std::unordered_set<std::string> hidden_device_ids_;
    std::unordered_map<std::string, QDateTime> last_live_update_;
    std::vector<std::string> connected_device_order_;
    std::vector<std::string> disconnected_device_order_;
    std::string pending_bluetooth_refresh_device_id_;
    std::vector<DeviceBatteryInfo> last_devices_snapshot_;
    LowBatteryNotifier low_battery_notifier_;
    std::unordered_map<std::string, QPointer<BatteryHistoryDialog>> history_dialogs_;
    std::unordered_map<std::string, QPointer<BatteryStatsDialog>> stats_dialogs_;
    std::unordered_map<std::string, QPointer<DeviceDiagnosticsDialog>> diagnostics_dialogs_;
    std::vector<QPointer<QLabel>> runtime_labels_;
    std::unordered_map<std::string, qint64> runtime_deadline_ms_by_device_;
    std::unordered_map<std::string, std::string> runtime_state_key_by_device_;
    std::unordered_map<std::string, std::unordered_map<std::string, qint64>> runtime_deadline_ms_by_component_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> runtime_state_key_by_component_;
    std::jthread refresh_worker_;
    std::atomic<bool> refresh_in_progress_{false};
    bool active_force_live_refresh_ = false;
    std::string active_refresh_target_device_id_;
    bool refresh_pending_ = false;
    bool pending_force_live_refresh_ = false;
    bool pending_include_disconnected_ = false;
    bool pending_preserve_disconnected_snapshot_ = true;
    bool drag_in_progress_ = false;
    bool settings_panel_expanded_ = false;
    int refresh_interval_ms_ = kBatteryWindowDefaultRefreshIntervalMs;
    int low_battery_threshold_percent_ = kBatteryWindowDefaultLowBatteryThresholdPercent;
    int low_battery_repeat_minutes_ = kBatteryWindowDefaultLowBatteryRepeatMinutes;
    bool quitting_ = false;
    UpdateService* update_service_ = nullptr;
    QPointer<UpdateDialog> update_dialog_;
    BatteryHistoryStore history_store_;
};

}  // namespace battery_monitor
