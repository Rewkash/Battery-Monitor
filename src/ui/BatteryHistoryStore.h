#pragma once

#include <QHash>
#include <QMap>
#include <QString>
#include <QVector>

#include <vector>

#include "core/BatteryTypes.h"

namespace battery_monitor {

struct BatteryHistorySample {
    qint64 timestamp_ms = 0;
    QMap<QString, int> component_levels;
};

struct BatteryHistoryData {
    QString device_id;
    QString device_name;
    QVector<BatteryHistorySample> samples;
};

class BatteryHistoryStore {
   public:
    BatteryHistoryStore();

    void RecordSnapshot(const std::vector<DeviceBatteryInfo>& devices);
    BatteryHistoryData LoadHistory(const QString& device_id) const;

   private:
    void LoadFromDisk();
    void SaveToDisk() const;
    void PruneExpiredSamples(BatteryHistoryData* history) const;

    QHash<QString, BatteryHistoryData> history_by_device_;
};

}  // namespace battery_monitor
