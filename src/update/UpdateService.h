#pragma once

#include <QObject>

#include "update/UpdateManifest.h"

class QNetworkAccessManager;
class QNetworkReply;

namespace battery_monitor {

class UpdateService final : public QObject {
    Q_OBJECT

   public:
    explicit UpdateService(QObject* parent = nullptr);
    void CheckForUpdates(bool user_initiated = false);
    void DownloadAndInstall(const UpdateManifest& manifest);

   signals:
    void CheckFinished(bool update_available, const UpdateManifest& manifest, const QString& error);
    void DownloadProgress(qint64 received, qint64 total);
    void InstallReady();
    void InstallFailed(const QString& error);

   private:
    void DownloadManifestSignature(const QByteArray& manifest_bytes, bool user_initiated);
    void FinishManifestCheck(const QByteArray& manifest_bytes,
                             const QByteArray& signature_bytes,
                             bool user_initiated);
    void FailCheck(bool user_initiated, const QString& error);

    QNetworkAccessManager* network_ = nullptr;
    QNetworkReply* active_reply_ = nullptr;
};

}  // namespace battery_monitor
