#include "update/UpdateService.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

#include "BatteryMonitorVersion.h"
#include "update/UpdateSecurity.h"
#include "update/UpdateState.h"

namespace battery_monitor {
namespace {

QNetworkRequest MakeRequest(const QUrl& url) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("BatteryMonitor/%1").arg(QStringLiteral(BATTERY_MONITOR_VERSION)));
    request.setTransferTimeout(15000);
    return request;
}

bool IsAllowedDownloadUrl(const QUrl& url) {
    const QString host = url.host().toLower();
    return url.scheme() == QStringLiteral("https") &&
           (host == QStringLiteral("github.com") ||
            host == QStringLiteral("objects.githubusercontent.com") ||
            host == QStringLiteral("release-assets.githubusercontent.com"));
}

QString UniqueToken() {
    auto* random = QRandomGenerator::system();
    return QStringLiteral("%1%2")
        .arg(random->generate64(), 16, 16, QLatin1Char('0'))
        .arg(random->generate64(), 16, 16, QLatin1Char('0'));
}

}  // namespace

UpdateService::UpdateService(QObject* parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)) {}

void UpdateService::CheckForUpdates(bool user_initiated) {
    if (active_reply_ != nullptr) {
        FailCheck(user_initiated, QStringLiteral("Проверка обновлений уже выполняется."));
        return;
    }
    active_reply_ = network_->get(MakeRequest(QUrl(QStringLiteral(BATTERY_MONITOR_UPDATE_MANIFEST_URL))));
    connect(active_reply_, &QNetworkReply::finished, this, [this, user_initiated]() {
        QNetworkReply* reply = active_reply_;
        active_reply_ = nullptr;
        const QByteArray bytes = reply->readAll();
        const QString error = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
        const bool allowed_url = IsAllowedDownloadUrl(reply->url());
        reply->deleteLater();
        if (!error.isEmpty() || !allowed_url) {
            FailCheck(user_initiated, allowed_url ? error : QStringLiteral("Недопустимое перенаправление манифеста."));
            return;
        }
        if (bytes.size() > kMaximumManifestBytes) {
            FailCheck(user_initiated, QStringLiteral("Манифест обновления слишком большой."));
            return;
        }
        DownloadManifestSignature(bytes, user_initiated);
    });
}

void UpdateService::DownloadManifestSignature(const QByteArray& manifest_bytes, bool user_initiated) {
    active_reply_ = network_->get(MakeRequest(QUrl(QStringLiteral(BATTERY_MONITOR_UPDATE_SIGNATURE_URL))));
    connect(active_reply_, &QNetworkReply::finished, this, [this, manifest_bytes, user_initiated]() {
        QNetworkReply* reply = active_reply_;
        active_reply_ = nullptr;
        const QByteArray signature = reply->readAll();
        const QString error = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
        const bool allowed_url = IsAllowedDownloadUrl(reply->url());
        reply->deleteLater();
        if (!error.isEmpty() || !allowed_url || signature.size() > 256) {
            FailCheck(user_initiated, !allowed_url ? QStringLiteral("Недопустимое перенаправление подписи.")
                                                   : (signature.size() > 256 ? QStringLiteral("Подпись слишком большая.") : error));
            return;
        }
        FinishManifestCheck(manifest_bytes, signature, user_initiated);
    });
}

void UpdateService::FinishManifestCheck(const QByteArray& manifest_bytes,
                                        const QByteArray& signature_bytes,
                                        bool user_initiated) {
    QString error;
    if (!VerifyUpdateManifestSignature(manifest_bytes, signature_bytes, &error)) {
        FailCheck(user_initiated, error);
        return;
    }
    UpdateManifest manifest;
    if (!ParseAndValidateUpdateManifest(manifest_bytes, &manifest, &error)) {
        FailCheck(user_initiated, error);
        return;
    }
    const std::uint64_t highest_sequence = LoadHighestAcceptedUpdateSequence();
    if (manifest.sequence < highest_sequence) {
        FailCheck(user_initiated, QStringLiteral("Подписанный манифест пытается понизить sequence."));
        return;
    }
    if (manifest.sequence == highest_sequence &&
        CompareSemanticVersions(manifest.version, QStringLiteral(BATTERY_MONITOR_VERSION)) > 0) {
        FailCheck(user_initiated, QStringLiteral("Новая версия не может повторно использовать установленный sequence."));
        return;
    }
    emit CheckFinished(CompareSemanticVersions(manifest.version, QStringLiteral(BATTERY_MONITOR_VERSION)) > 0,
                       manifest, QString());
}

void UpdateService::FailCheck(bool user_initiated, const QString& error) {
    emit CheckFinished(false, UpdateManifest{}, user_initiated ? error : QString());
}

void UpdateService::DownloadAndInstall(const UpdateManifest& manifest) {
    if (active_reply_ != nullptr || !IsAllowedDownloadUrl(manifest.artifact_url)) {
        emit InstallFailed(QStringLiteral("Недопустимый или уже выполняющийся запрос обновления."));
        return;
    }
    const QString root_path = ResolveUpdateDataRoot();
    QDir root(root_path);
    if (!root.mkpath(QStringLiteral("downloads"))) {
        emit InstallFailed(QStringLiteral("Не удалось создать каталог обновлений."));
        return;
    }
    const QString token = UniqueToken();
    const QString package_path = root.filePath(QStringLiteral("downloads/%1.bmup").arg(token));

    active_reply_ = network_->get(MakeRequest(manifest.artifact_url));
    auto* output = new QSaveFile(package_path, active_reply_);
    if (!output->open(QIODevice::WriteOnly)) {
        active_reply_->abort();
        active_reply_->deleteLater();
        active_reply_ = nullptr;
        emit InstallFailed(QStringLiteral("Не удалось создать файл загрузки."));
        return;
    }
    connect(active_reply_, &QNetworkReply::readyRead, this, [this, output, manifest]() {
        const QByteArray chunk = active_reply_->readAll();
        if (output->pos() + chunk.size() > static_cast<qint64>(manifest.artifact_size) ||
            output->write(chunk) != chunk.size()) {
            active_reply_->abort();
        }
    });
    connect(active_reply_, &QNetworkReply::downloadProgress, this, &UpdateService::DownloadProgress);
    connect(active_reply_, &QNetworkReply::finished, this, [this, output, package_path, manifest, root_path, token]() {
        QNetworkReply* reply = active_reply_;
        active_reply_ = nullptr;
        const bool network_ok = reply->error() == QNetworkReply::NoError;
        const bool allowed_url = IsAllowedDownloadUrl(reply->url());
        const QString network_error = reply->errorString();
        reply->deleteLater();
        if (!network_ok || !allowed_url || !output->commit()) {
            emit InstallFailed(QStringLiteral("Загрузка обновления не завершена: %1").arg(network_error));
            return;
        }

        QString error;
        if (!VerifySha256(package_path, manifest.artifact_sha256, manifest.artifact_size, &error)) {
            emit InstallFailed(error);
            return;
        }
        const QString install_directory = QCoreApplication::applicationDirPath();
        QDir install_parent(QFileInfo(install_directory).absolutePath());
        const QString staging_name = QStringLiteral(".battery-monitor-stage-%1").arg(token);
        const QString backup_name = QStringLiteral(".battery-monitor-backup-%1").arg(token);
        const QString staging_path = install_parent.filePath(staging_name);
        const QString backup_directory = install_parent.filePath(backup_name);
        if (QFileInfo::exists(staging_path) || QFileInfo::exists(backup_directory) ||
            !install_parent.mkdir(staging_name)) {
            emit InstallFailed(QStringLiteral("Не удалось подготовить staging рядом с приложением."));
            return;
        }
        if (!ExtractVerifiedUpdateBundle(package_path, staging_path, &error)) {
            QDir(staging_path).removeRecursively();
            emit InstallFailed(error);
            return;
        }
        const QStringList required_files = {
            QStringLiteral("battery-monitor.exe"), QStringLiteral("battery-monitor-cli.exe"),
            QStringLiteral("battery-monitor-maintenance.exe")};
        for (const QString& required_file : required_files) {
            const QFileInfo staged_file(QDir(staging_path).filePath(required_file));
            if (!staged_file.isFile() || staged_file.isSymLink()) {
                QDir(staging_path).removeRecursively();
                emit InstallFailed(QStringLiteral("Пакет обновления неполон: %1").arg(required_file));
                return;
            }
        }

        const QString maintenance_source = QDir(QCoreApplication::applicationDirPath())
                                               .filePath(QStringLiteral("battery-monitor-maintenance.exe"));
        const QString maintenance_copy = QDir(root_path).filePath(QStringLiteral("maintenance-%1.exe").arg(token));
        QFile::remove(maintenance_copy);
        if (!QFile::copy(maintenance_source, maintenance_copy)) {
            emit InstallFailed(QStringLiteral("Maintenance tool отсутствует в сборке."));
            return;
        }
        const QString executable_name = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
        const QString state_path = QDir(ResolveUpdateDataRoot()).filePath(QStringLiteral("state.json"));
        const QStringList arguments = {
            QStringLiteral("--apply"), install_directory, staging_path, backup_directory,
            executable_name, QString::number(QCoreApplication::applicationPid()), token,
            QString::number(manifest.sequence), state_path};
        if (!QProcess::startDetached(maintenance_copy, arguments, root_path)) {
            emit InstallFailed(QStringLiteral("Не удалось запустить maintenance tool."));
            return;
        }
        emit InstallReady();
    });
}

}  // namespace battery_monitor
