#pragma once

#include <optional>

#include <QHash>
#include <QString>

#include "ui/BatteryHistoryStore.h"

namespace battery_monitor {

enum class BatteryRuntimeConfidence {
    Low,
    Medium,
    High,
};

struct BatteryComponentRuntimeEstimate {
    QString component_key;
    std::optional<qint64> remaining_ms;
    BatteryRuntimeConfidence confidence = BatteryRuntimeConfidence::Low;
};

struct BatteryRuntimeForecast {
    QHash<QString, BatteryComponentRuntimeEstimate> by_component;
    std::optional<qint64> pair_remaining_ms;
    BatteryRuntimeConfidence pair_confidence = BatteryRuntimeConfidence::Low;
};

BatteryRuntimeForecast EstimateBatteryRuntimeForecast(const BatteryHistoryData& history);
QString FormatRuntimeDurationCompact(qint64 duration_ms);
QString BuildRuntimeForecastSummary(const BatteryRuntimeForecast& forecast);
QString BuildRuntimeForecastCompactSummary(const BatteryRuntimeForecast& forecast);
QString BuildBatteryStatisticsReport(const BatteryHistoryData& history);

}  // namespace battery_monitor
