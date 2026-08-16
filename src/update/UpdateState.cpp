#include "update/UpdateState.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>

namespace battery_monitor {
namespace {

// A committed state file is a few hundred bytes; anything larger is corrupt.
constexpr qint64 kMaximumStateFileBytes = 64 * 1024;

QString StateFilePath() {
    return QDir(ResolveUpdateDataRoot()).filePath(QStringLiteral("state.json"));
}

QString StateBackupFilePath() {
    return QDir(ResolveUpdateDataRoot()).filePath(QStringLiteral("state.json.bak"));
}

// Parses a state file. Returns false when the file is unreadable, oversized, or
// not valid JSON (i.e. corrupt), so callers can fall back to the backup copy.
bool ParseStateFile(const QString& path, std::uint64_t* sequence) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaximumStateFileBytes) {
        return false;
    }
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    const QJsonValue value = document.object().value(QStringLiteral("highestSequence"));
    bool ok = false;
    const qulonglong parsed = value.isString() ? value.toString().toULongLong(&ok) : 0;
    if (ok) {
        *sequence = static_cast<std::uint64_t>(parsed);
        return true;
    }
    const double legacy = value.toDouble(0);
    if (legacy > 0 && legacy <= 9007199254740991.0) {
        *sequence = static_cast<std::uint64_t>(legacy);
        return true;
    }
    *sequence = 0;
    return true;
}

}  // namespace

QString ResolveUpdateDataRoot() {
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty()) {
        root = QDir::home().filePath(QStringLiteral(".battery-monitor"));
    }
    return QDir(root).filePath(QStringLiteral("updates"));
}

std::uint64_t LoadHighestAcceptedUpdateSequence() {
    std::uint64_t sequence = 0;
    if (ParseStateFile(StateFilePath(), &sequence)) {
        return sequence;
    }
    // The primary state file is corrupt: keep it for diagnosis instead of
    // silently resetting anti-rollback state, and recover from the backup.
    QFile::remove(StateFilePath() + QStringLiteral(".corrupt"));
    QFile::rename(StateFilePath(), StateFilePath() + QStringLiteral(".corrupt"));
    if (ParseStateFile(StateBackupFilePath(), &sequence)) {
        return sequence;
    }
    return 0;
}

bool SaveHighestAcceptedUpdateSequence(std::uint64_t sequence, QString* error) {
    // Anti-rollback state must never regress, even if the caller passes a
    // stale value.
    sequence = std::max(sequence, LoadHighestAcceptedUpdateSequence());

    QDir root(ResolveUpdateDataRoot());
    if (!root.mkpath(QStringLiteral("."))) {
        if (error != nullptr) *error = QStringLiteral("Cannot create update state directory.");
        return false;
    }
    // Refresh the backup from the current file so recovery after a future
    // corrupted write has the last known-good state.
    std::uint64_t previous = 0;
    if (ParseStateFile(StateFilePath(), &previous)) {
        QFile::remove(StateBackupFilePath());
        QFile::copy(StateFilePath(), StateBackupFilePath());
    }
    QSaveFile file(StateFilePath());
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) *error = QStringLiteral("Cannot write update state.");
        return false;
    }
    QJsonObject object;
    object.insert(QStringLiteral("schemaVersion"), 1);
    object.insert(QStringLiteral("highestSequence"), QString::number(sequence));
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    return file.commit();
}

}  // namespace battery_monitor
