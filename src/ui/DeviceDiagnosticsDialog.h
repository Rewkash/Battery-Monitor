#pragma once

#include <vector>

#include <QDialog>
#include <QString>

#include "core/BatteryTypes.h"

class QLabel;
class QTextBrowser;

namespace battery_monitor {

class DeviceDiagnosticsDialog final : public QDialog {
   public:
    explicit DeviceDiagnosticsDialog(std::vector<DeviceBatteryInfo> entries, QWidget* parent = nullptr);

    QString log_file_path() const { return log_file_path_; }

   private:
    void BuildUi();
    QString BuildDiagnosticsText() const;
    QString WriteDiagnosticsLog(const QString& diagnostics_text) const;

    std::vector<DeviceBatteryInfo> entries_;
    QString log_file_path_;
    QLabel* title_label_ = nullptr;
    QLabel* log_path_label_ = nullptr;
    QTextBrowser* diagnostics_view_ = nullptr;
};

}  // namespace battery_monitor
