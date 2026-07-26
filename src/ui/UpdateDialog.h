#pragma once

#include <QDialog>

#include "update/UpdateManifest.h"

class QLabel;
class QProgressBar;
class QPushButton;
class QTextBrowser;

namespace battery_monitor {

class UpdateDialog final : public QDialog {
    Q_OBJECT

   public:
    explicit UpdateDialog(const UpdateManifest& manifest, QWidget* parent = nullptr);
    void SetDownloadProgress(qint64 received, qint64 total);
    void SetInstalling();
    void SetError(const QString& error);

   signals:
    void InstallRequested();

   private:
    UpdateManifest manifest_;
    QLabel* status_label_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* install_button_ = nullptr;
    QPushButton* later_button_ = nullptr;
};

}  // namespace battery_monitor
