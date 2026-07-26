#include "update/UpdateState.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace battery_monitor {

QString ResolveUpdateDataRoot() {
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty()) {
        root = QDir::home().filePath(QStringLiteral(".battery-monitor"));
    }
    return QDir(root).filePath(QStringLiteral("updates"));
}

std::uint64_t LoadHighestAcceptedUpdateSequence() {
    QFile file(QDir(ResolveUpdateDataRoot()).filePath(QStringLiteral("state.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        return 0;
    }
    const QJsonValue value = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("highestSequence"));
    bool ok = false;
    const qulonglong sequence = value.isString() ? value.toString().toULongLong(&ok) : 0;
    if (ok) {
        return static_cast<std::uint64_t>(sequence);
    }
    const double legacy = value.toDouble(0);
    return legacy > 0 && legacy <= 9007199254740991.0
               ? static_cast<std::uint64_t>(legacy)
               : 0;
}

bool SaveHighestAcceptedUpdateSequence(std::uint64_t sequence, QString* error) {
    QDir root(ResolveUpdateDataRoot());
    if (!root.mkpath(QStringLiteral("."))) {
        if (error != nullptr) *error = QStringLiteral("Cannot create update state directory.");
        return false;
    }
    QSaveFile file(root.filePath(QStringLiteral("state.json")));
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
