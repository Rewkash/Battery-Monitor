#pragma once

#include <cstdint>

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUrl>

namespace battery_monitor {

struct UpdateManifest {
    int schema_version = 0;
    std::uint64_t sequence = 0;
    QString version;
    QString channel;
    QString release_notes;
    QUrl release_notes_url;
    QDateTime published_at;
    QDateTime expires_at;
    bool mandatory = false;
    QUrl artifact_url;
    std::uint64_t artifact_size = 0;
    QByteArray artifact_sha256;
    QString artifact_format;
};

[[nodiscard]] bool ParseAndValidateUpdateManifest(const QByteArray& bytes,
                                                  UpdateManifest* manifest,
                                                  QString* error);
[[nodiscard]] int CompareSemanticVersions(const QString& lhs, const QString& rhs);

}  // namespace battery_monitor
