#pragma once

#include <memory>
#include <string>
#include <vector>

#include <QWidget>

#include "core/BatteryTypes.h"
#include "core/IBluetoothBatteryProvider.h"

class QLabel;
class QPushButton;
class QVBoxLayout;
class QWidget;

namespace battery_monitor {

class BatteryWindow : public QWidget {
   public:
    explicit BatteryWindow(std::unique_ptr<IBluetoothBatteryProvider> provider, QWidget* parent = nullptr);

   private:
    void RefreshBatteryData();
    void PopulateDeviceCards(const std::vector<DeviceBatteryInfo>& devices);
    void ClearDeviceCards();
    static std::string FormatError(const std::exception& ex);

    std::unique_ptr<IBluetoothBatteryProvider> provider_;
    QWidget* cards_container_ = nullptr;
    QVBoxLayout* cards_layout_ = nullptr;
    QPushButton* refresh_button_ = nullptr;
    QLabel* summary_label_ = nullptr;
    QLabel* status_label_ = nullptr;
};

}  // namespace battery_monitor
