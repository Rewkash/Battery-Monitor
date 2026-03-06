#include "ui/BatteryHistoryStore.h"

#include <algorithm>
#include <cctype>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace battery_monitor {

namespace {

struct PendingHistorySnapshot {
    QString device_name;
    QMap<QString, int> component_levels;
};

constexpr int kHistorySchemaVersion = 1;
constexpr qint64 kHistoryRetentionMs = 14LL * 24LL * 60LL * 60LL * 1000LL;
constexpr qint64 kHistoryMinSampleIntervalMs = 10LL * 60LL * 1000LL;
constexpr int kMaxSamplesPerDevice = 2048;

QString NormalizeHistoryComponent(const std::string& component) {
    if (component.empty()) {
        return QStringLiteral("main");
    }

    std::string normalized = component;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return QString::fromUtf8(normalized.c_str());
}

QString ResolveHistoryFilePath() {
    QString base_path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base_path.trimmed().isEmpty()) {
        base_path = QDir::home().filePath(QStringLiteral(".battery-monitor"));
    }

    QDir directory(base_path);
    directory.mkpath(QStringLiteral("."));
    return directory.filePath(QStringLiteral("battery-history.json"));
}

bool ShouldTrackEntry(const DeviceBatteryInfo& entry) {
    return entry.is_connected && !entry.is_cached && entry.battery_level_percent.has_value() && !entry.device_id.empty();
}

bool ShouldAppendSample(const QVector<BatteryHistorySample>& samples, const BatteryHistorySample& sample) {
    if (samples.isEmpty()) {
        return true;
    }

    const auto& previous = samples.back();
    if (previous.component_levels != sample.component_levels) {
        return true;
    }

    return (sample.timestamp_ms - previous.timestamp_ms) >= kHistoryMinSampleIntervalMs;
}

QJsonObject SerializeSample(const BatteryHistorySample& sample) {
    QJsonObject sample_object;
    sample_object.insert(QStringLiteral("ts"), sample.timestamp_ms);

    QJsonObject components_object;
    for (auto it = sample.component_levels.cbegin(); it != sample.component_levels.cend(); ++it) {
        components_object.insert(it.key(), it.value());
    }
    sample_object.insert(QStringLiteral("components"), components_object);
    return sample_object;
}

bool ParseSample(const QJsonObject& sample_object, BatteryHistorySample* sample) {
    if (sample == nullptr) {
        return false;
    }

    const qint64 timestamp_ms = static_cast<qint64>(sample_object.value(QStringLiteral("ts")).toDouble(0));
    if (timestamp_ms <= 0) {
        return false;
    }

    const auto components_value = sample_object.value(QStringLiteral("components"));
    if (!components_value.isObject()) {
        return false;
    }

    QMap<QString, int> component_levels;
    const auto components_object = components_value.toObject();
    for (auto it = components_object.begin(); it != components_object.end(); ++it) {
        component_levels.insert(it.key(), std::clamp(it.value().toInt(-1), 0, 100));
    }

    if (component_levels.isEmpty()) {
        return false;
    }

    sample->timestamp_ms = timestamp_ms;
    sample->component_levels = std::move(component_levels);
    return true;
}

}  // namespace

BatteryHistoryStore::BatteryHistoryStore() {
    LoadFromDisk();
}

void BatteryHistoryStore::RecordSnapshot(const std::vector<DeviceBatteryInfo>& devices) {
    QHash<QString, PendingHistorySnapshot> pending_by_device;
    for (const auto& entry : devices) {
        if (!ShouldTrackEntry(entry)) {
            continue;
        }

        const QString device_id = QString::fromUtf8(entry.device_id.c_str());
        if (device_id.trimmed().isEmpty()) {
            continue;
        }

        auto& snapshot = pending_by_device[device_id];
        if (snapshot.device_name.isEmpty()) {
            snapshot.device_name = QString::fromUtf8(entry.device_name.c_str());
        }
        snapshot.component_levels.insert(NormalizeHistoryComponent(entry.battery_component),
                                         static_cast<int>(*entry.battery_level_percent));
    }

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    bool is_dirty = false;

    for (auto it = pending_by_device.begin(); it != pending_by_device.end(); ++it) {
        auto& history = history_by_device_[it.key()];
        history.device_id = it.key();

        if (!it->device_name.trimmed().isEmpty() && history.device_name != it->device_name) {
            history.device_name = it->device_name;
            is_dirty = true;
        }

        PruneExpiredSamples(&history);

        BatteryHistorySample sample;
        sample.timestamp_ms = now_ms;
        sample.component_levels = it->component_levels;

        if (ShouldAppendSample(history.samples, sample)) {
            history.samples.push_back(std::move(sample));
            is_dirty = true;
        }

        PruneExpiredSamples(&history);
    }

    for (auto it = history_by_device_.begin(); it != history_by_device_.end();) {
        PruneExpiredSamples(&it.value());
        if (it.value().samples.isEmpty() && it.value().device_name.trimmed().isEmpty()) {
            it = history_by_device_.erase(it);
            is_dirty = true;
            continue;
        }
        ++it;
    }

    if (is_dirty) {
        SaveToDisk();
    }
}

BatteryHistoryData BatteryHistoryStore::LoadHistory(const QString& device_id) const {
    const auto history_it = history_by_device_.find(device_id);
    if (history_it != history_by_device_.end()) {
        return history_it.value();
    }

    BatteryHistoryData history;
    history.device_id = device_id;
    return history;
}

void BatteryHistoryStore::LoadFromDisk() {
    history_by_device_.clear();

    QFile file(ResolveHistoryFilePath());
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return;
    }

    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        return;
    }

    const auto root_object = document.object();
    const auto devices_value = root_object.value(QStringLiteral("devices"));
    if (!devices_value.isArray()) {
        return;
    }

    const auto device_array = devices_value.toArray();
    for (const auto& device_value : device_array) {
        if (!device_value.isObject()) {
            continue;
        }

        const auto device_object = device_value.toObject();
        const QString device_id = device_object.value(QStringLiteral("deviceId")).toString();
        if (device_id.trimmed().isEmpty()) {
            continue;
        }

        BatteryHistoryData history;
        history.device_id = device_id;
        history.device_name = device_object.value(QStringLiteral("deviceName")).toString();

        const auto samples_value = device_object.value(QStringLiteral("samples"));
        if (samples_value.isArray()) {
            const auto sample_array = samples_value.toArray();
            for (const auto& sample_value : sample_array) {
                if (!sample_value.isObject()) {
                    continue;
                }

                BatteryHistorySample sample;
                if (ParseSample(sample_value.toObject(), &sample)) {
                    history.samples.push_back(std::move(sample));
                }
            }
        }

        std::sort(history.samples.begin(), history.samples.end(),
                  [](const BatteryHistorySample& left, const BatteryHistorySample& right) {
                      return left.timestamp_ms < right.timestamp_ms;
                  });
        PruneExpiredSamples(&history);

        if (!history.samples.isEmpty() || !history.device_name.trimmed().isEmpty()) {
            history_by_device_.insert(device_id, std::move(history));
        }
    }
}

void BatteryHistoryStore::SaveToDisk() const {
    QJsonObject root_object;
    root_object.insert(QStringLiteral("version"), kHistorySchemaVersion);

    QStringList device_ids = history_by_device_.keys();
    device_ids.sort(Qt::CaseInsensitive);

    QJsonArray devices_array;
    for (const auto& device_id : device_ids) {
        const auto history_it = history_by_device_.find(device_id);
        if (history_it == history_by_device_.end()) {
            continue;
        }

        const auto& history = history_it.value();
        if (history.samples.isEmpty() && history.device_name.trimmed().isEmpty()) {
            continue;
        }

        QJsonObject device_object;
        device_object.insert(QStringLiteral("deviceId"), history.device_id);
        device_object.insert(QStringLiteral("deviceName"), history.device_name);

        QJsonArray samples_array;
        for (const auto& sample : history.samples) {
            samples_array.push_back(SerializeSample(sample));
        }
        device_object.insert(QStringLiteral("samples"), samples_array);
        devices_array.push_back(device_object);
    }

    root_object.insert(QStringLiteral("devices"), devices_array);

    QSaveFile file(ResolveHistoryFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }

    file.write(QJsonDocument(root_object).toJson(QJsonDocument::Indented));
    file.commit();
}

void BatteryHistoryStore::PruneExpiredSamples(BatteryHistoryData* history) const {
    if (history == nullptr) {
        return;
    }

    const qint64 cutoff_timestamp = QDateTime::currentMSecsSinceEpoch() - kHistoryRetentionMs;
    while (!history->samples.isEmpty() && history->samples.front().timestamp_ms < cutoff_timestamp) {
        history->samples.removeFirst();
    }

    const int overflow = history->samples.size() - kMaxSamplesPerDevice;
    if (overflow > 0) {
        history->samples.remove(0, overflow);
    }
}

}  // namespace battery_monitor
