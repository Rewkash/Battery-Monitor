#pragma once

#include <QPair>
#include <QString>
#include <QVector>

namespace battery_monitor {

enum class NoiseControlModeLabelStyle {
    Default,
    Compact,
};

QString NormalizeNoiseToken(const QString& value);
QString NoiseModeLabel(const QString& mode, NoiseControlModeLabelStyle style = NoiseControlModeLabelStyle::Default);
QString NoiseSubmodeLabel(const QString& submode);
bool NoiseModeNeedsSubmode(const QString& mode);
QString DefaultNoiseSubmodeToken(const QString& mode);
QVector<QPair<QString, QString>> NoiseSubmodeItems(const QString& mode);

}  // namespace battery_monitor
