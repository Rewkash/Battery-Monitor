#include "update/UpdateManifest.h"

#include <limits>
#include <algorithm>
#include <cmath>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>

#include "update/UpdateSecurity.h"

namespace battery_monitor {
namespace {

constexpr int kMaximumReleaseNotesBytes = 48 * 1024;

bool Fail(QString* error, const QString& message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool IsAllowedArtifactHost(const QUrl& url) {
    if (url.scheme() != QStringLiteral("https")) {
        return false;
    }
    const QString host = url.host().toLower();
    return host == QStringLiteral("github.com") ||
           host == QStringLiteral("objects.githubusercontent.com") ||
           host == QStringLiteral("release-assets.githubusercontent.com");
}

QList<int> ParseVersion(const QString& version) {
    const QString core = version.section(QLatin1Char('-'), 0, 0);
    const QStringList parts = core.split(QLatin1Char('.'));
    QList<int> result;
    for (const QString& part : parts) {
        bool ok = false;
        const int value = part.toInt(&ok);
        if (!ok || value < 0) {
            return {};
        }
        result.push_back(value);
    }
    while (result.size() < 3) {
        result.push_back(0);
    }
    return result;
}

}  // namespace

bool ParseAndValidateUpdateManifest(const QByteArray& bytes,
                                    UpdateManifest* manifest,
                                    QString* error) {
    if (manifest == nullptr) {
        return Fail(error, QStringLiteral("Manifest output is null."));
    }
    if (bytes.isEmpty() || bytes.size() > kMaximumManifestBytes) {
        return Fail(error, QStringLiteral("Manifest size is invalid."));
    }

    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return Fail(error, QStringLiteral("Manifest JSON is invalid: %1").arg(parse_error.errorString()));
    }
    const QJsonObject root = document.object();
    const QJsonObject artifact = root.value(QStringLiteral("artifact")).toObject();

    UpdateManifest parsed;
    parsed.schema_version = root.value(QStringLiteral("schemaVersion")).toInt(-1);
    parsed.sequence = static_cast<std::uint64_t>(root.value(QStringLiteral("sequence")).toDouble(-1));
    parsed.version = root.value(QStringLiteral("version")).toString();
    parsed.channel = root.value(QStringLiteral("channel")).toString();
    parsed.release_notes = root.value(QStringLiteral("releaseNotes")).toString();
    parsed.release_notes_url = QUrl(root.value(QStringLiteral("releaseNotesUrl")).toString());
    parsed.published_at = QDateTime::fromString(root.value(QStringLiteral("publishedAt")).toString(), Qt::ISODate);
    parsed.expires_at = QDateTime::fromString(root.value(QStringLiteral("expiresAt")).toString(), Qt::ISODate);
    parsed.mandatory = root.value(QStringLiteral("mandatory")).toBool(false);
    parsed.artifact_url = QUrl(artifact.value(QStringLiteral("url")).toString());
    parsed.artifact_size = static_cast<std::uint64_t>(artifact.value(QStringLiteral("size")).toDouble(-1));
    parsed.artifact_sha256 = QByteArray::fromHex(artifact.value(QStringLiteral("sha256")).toString().toLatin1());
    parsed.artifact_format = artifact.value(QStringLiteral("format")).toString();

    static const QRegularExpression version_pattern(QStringLiteral(R"(^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$)"));
    if (parsed.schema_version != 1 || parsed.sequence == 0 ||
        !version_pattern.match(parsed.version).hasMatch() || parsed.channel != QStringLiteral("stable")) {
        return Fail(error, QStringLiteral("Manifest identity fields are invalid."));
    }
    if (!parsed.published_at.isValid() || !parsed.expires_at.isValid() ||
        parsed.expires_at <= QDateTime::currentDateTimeUtc()) {
        return Fail(error, QStringLiteral("Manifest is expired or has invalid timestamps."));
    }
    if (parsed.release_notes.toUtf8().size() > kMaximumReleaseNotesBytes ||
        (!parsed.release_notes_url.isEmpty() &&
         (parsed.release_notes_url.scheme() != QStringLiteral("https") ||
          parsed.release_notes_url.host().toLower() != QStringLiteral("github.com")))) {
        return Fail(error, QStringLiteral("Manifest release notes are invalid."));
    }
    if (!IsAllowedArtifactHost(parsed.artifact_url) ||
        parsed.artifact_size == 0 || parsed.artifact_size > kMaximumPackageBytes ||
        parsed.artifact_sha256.size() != 32 || parsed.artifact_format != QStringLiteral("bmup-1")) {
        return Fail(error, QStringLiteral("Manifest artifact is invalid."));
    }

    *manifest = parsed;
    return true;
}

int CompareSemanticVersions(const QString& lhs, const QString& rhs) {
    const QList<int> left = ParseVersion(lhs);
    const QList<int> right = ParseVersion(rhs);
    if (left.isEmpty() || right.isEmpty()) {
        return QString::compare(lhs, rhs, Qt::CaseInsensitive);
    }
    for (int index = 0; index < std::max(left.size(), right.size()); ++index) {
        const int left_value = index < left.size() ? left[index] : 0;
        const int right_value = index < right.size() ? right[index] : 0;
        if (left_value != right_value) {
            return left_value < right_value ? -1 : 1;
        }
    }
    return 0;
}

}  // namespace battery_monitor
