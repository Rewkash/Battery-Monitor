#include "ui/BatteryHistoryStore.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include <QDateTime>
#include <QDebug>
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
    QString device_mode;
    QString device_submode;
    QMap<QString, int> component_levels;
};

constexpr int kHistorySchemaVersion = 3;
constexpr qint64 kHistoryRetentionMs = 14LL * 24LL * 60LL * 60LL * 1000LL;
constexpr qint64 kHistoryMinSampleIntervalMs = 10LL * 60LL * 1000LL;
// Samples inside the recent window are kept as-is; older samples are
// downsampled into hourly buckets once the per-device cap is exceeded.
constexpr qint64 kHistoryRawWindowMs = 2LL * 24LL * 60LL * 60LL * 1000LL;
constexpr qint64 kHistoryBucketMs = 60LL * 60LL * 1000LL;
constexpr int kMaxSamplesPerDevice = 2048;
// Hard cap for the on-disk history file; when exceeded, older data is dropped
// first and, if that is not enough, the write is skipped with a diagnostic.
constexpr qint64 kMaxHistoryFileBytes = 8LL * 1024LL * 1024LL;

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

QString ResolveHistoryBackupFilePath(const QString& primary_path) {
    return primary_path + QStringLiteral(".bak");
}

QString ResolveHistoryCorruptFilePath(const QString& primary_path) {
    return primary_path + QStringLiteral(".corrupt-") +
           QString::number(QDateTime::currentMSecsSinceEpoch());
}

bool ShouldTrackEntry(const DeviceBatteryInfo& entry) {
    return entry.is_connected && !entry.is_cached && entry.battery_level_percent.has_value() && !entry.device_id.empty();
}

bool ShouldAppendSample(const QVector<BatteryHistorySample>& samples, const BatteryHistorySample& sample) {
    if (samples.isEmpty()) {
        return true;
    }

    const auto& previous = samples.back();
    if (previous.offline != sample.offline) {
        return true;
    }
    if (sample.offline && previous.offline) {
        // Only one offline marker per offline period.
        return false;
    }
    if (previous.component_levels != sample.component_levels) {
        return true;
    }
    if (previous.device_mode != sample.device_mode || previous.device_submode != sample.device_submode) {
        return true;
    }

    return (sample.timestamp_ms - previous.timestamp_ms) >= kHistoryMinSampleIntervalMs;
}

QJsonObject SerializeSample(const BatteryHistorySample& sample) {
    QJsonObject sample_object;
    sample_object.insert(QStringLiteral("ts"), sample.timestamp_ms);
    if (sample.offline) {
        sample_object.insert(QStringLiteral("offline"), true);
    }

    QJsonObject components_object;
    for (auto it = sample.component_levels.cbegin(); it != sample.component_levels.cend(); ++it) {
        components_object.insert(it.key(), it.value());
    }
    sample_object.insert(QStringLiteral("components"), components_object);
    if (!sample.device_mode.trimmed().isEmpty()) {
        sample_object.insert(QStringLiteral("mode"), sample.device_mode);
    }
    if (!sample.device_submode.trimmed().isEmpty()) {
        sample_object.insert(QStringLiteral("submode"), sample.device_submode);
    }
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

    const bool offline = sample_object.value(QStringLiteral("offline")).toBool(false);

    QMap<QString, int> component_levels;
    const auto components_value = sample_object.value(QStringLiteral("components"));
    if (components_value.isObject()) {
        const auto components_object = components_value.toObject();
        for (auto it = components_object.begin(); it != components_object.end(); ++it) {
            component_levels.insert(it.key(), std::clamp(it.value().toInt(-1), 0, 100));
        }
    }

    if (component_levels.isEmpty() && !offline) {
        return false;
    }

    sample->timestamp_ms = timestamp_ms;
    sample->offline = offline;
    sample->component_levels = std::move(component_levels);
    sample->device_mode = sample_object.value(QStringLiteral("mode")).toString().trimmed();
    sample->device_submode = sample_object.value(QStringLiteral("submode")).toString().trimmed();
    return true;
}

QJsonDocument SerializeHistory(const QHash<QString, BatteryHistoryData>& history_by_device) {
    QJsonObject root_object;
    root_object.insert(QStringLiteral("version"), kHistorySchemaVersion);

    QStringList device_ids = history_by_device.keys();
    device_ids.sort(Qt::CaseInsensitive);

    QJsonArray devices_array;
    for (const auto& device_id : device_ids) {
        const auto history_it = history_by_device.find(device_id);
        if (history_it == history_by_device.end()) {
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
    return QJsonDocument(root_object);
}

// Replaces blind truncation of old samples with hourly aggregation: samples
// older than the raw window are averaged into one point per hour, while
// offline markers act as bucket boundaries and are preserved as-is.
void DownsampleHistory(BatteryHistoryData* history, qint64 now_ms) {
    if (history == nullptr || history->samples.size() <= kMaxSamplesPerDevice) {
        return;
    }

    const qint64 raw_cutoff_ms = now_ms - kHistoryRawWindowMs;
    int raw_start_index = history->samples.size();
    while (raw_start_index > 0 && history->samples[raw_start_index - 1].timestamp_ms >= raw_cutoff_ms) {
        --raw_start_index;
    }
    if (raw_start_index <= 0) {
        return;
    }

    QVector<BatteryHistorySample> aggregated;
    aggregated.reserve(raw_start_index / 2 + 1);

    QMap<QString, std::pair<int, int>> bucket_totals;  // component -> (sum, count)
    BatteryHistorySample bucket_tail;
    qint64 bucket_start_ms = 0;
    bool bucket_open = false;

    auto flush_bucket = [&]() {
        if (!bucket_open) {
            return;
        }
        BatteryHistorySample aggregated_sample;
        aggregated_sample.timestamp_ms = bucket_tail.timestamp_ms;
        aggregated_sample.device_mode = bucket_tail.device_mode;
        aggregated_sample.device_submode = bucket_tail.device_submode;
        for (auto it = bucket_totals.cbegin(); it != bucket_totals.cend(); ++it) {
            aggregated_sample.component_levels.insert(it.key(), it.value().first / it.value().second);
        }
        aggregated.push_back(std::move(aggregated_sample));
        bucket_open = false;
        bucket_totals.clear();
    };

    for (int index = 0; index < raw_start_index; ++index) {
        const auto& sample = history->samples[index];
        if (sample.offline) {
            flush_bucket();
            aggregated.push_back(sample);
            continue;
        }

        const qint64 bucket_id = sample.timestamp_ms / kHistoryBucketMs;
        if (!bucket_open || bucket_id != bucket_start_ms) {
            flush_bucket();
            bucket_start_ms = bucket_id;
            bucket_open = true;
        }
        for (auto it = sample.component_levels.cbegin(); it != sample.component_levels.cend(); ++it) {
            auto& total = bucket_totals[it.key()];
            total.first += it.value();
            total.second += 1;
        }
        bucket_tail = sample;
    }
    flush_bucket();

    if (aggregated.size() + (history->samples.size() - raw_start_index) >= history->samples.size()) {
        return;
    }

    for (int index = raw_start_index; index < history->samples.size(); ++index) {
        aggregated.push_back(history->samples[index]);
    }
    history->samples = std::move(aggregated);
}

}  // namespace

BatteryHistoryStore::BatteryHistoryStore() {
    LoadFromDisk();
}

void BatteryHistoryStore::RecordSnapshot(const std::vector<DeviceBatteryInfo>& devices) {
    QHash<QString, PendingHistorySnapshot> pending_by_device;
    QSet<QString> connected_devices;
    for (const auto& entry : devices) {
        const QString device_id = QString::fromUtf8(entry.device_id.c_str());
        if (device_id.trimmed().isEmpty()) {
            continue;
        }

        if (entry.is_connected) {
            connected_devices.insert(device_id);
        }

        if (!ShouldTrackEntry(entry)) {
            continue;
        }

        auto& snapshot = pending_by_device[device_id];
        if (snapshot.device_name.isEmpty()) {
            snapshot.device_name = QString::fromUtf8(entry.device_name.c_str());
        }
        if (!entry.device_mode.value_or(std::string()).empty()) {
            snapshot.device_mode = QString::fromUtf8(entry.device_mode->c_str()).trimmed();
        }
        if (!entry.device_submode.value_or(std::string()).empty()) {
            snapshot.device_submode = QString::fromUtf8(entry.device_submode->c_str()).trimmed();
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
        sample.device_mode = it->device_mode;
        sample.device_submode = it->device_submode;

        if (ShouldAppendSample(history.samples, sample)) {
            history.samples.push_back(std::move(sample));
            is_dirty = true;
        }

        PruneExpiredSamples(&history);
    }

    // Record disconnects explicitly so cached values are never mistaken for
    // live readings when the history is rendered.
    for (auto it = history_by_device_.begin(); it != history_by_device_.end(); ++it) {
        auto& history = it.value();
        if (!connected_devices.contains(it.key()) && !history.samples.isEmpty() &&
            !history.samples.back().offline) {
            BatteryHistorySample offline_sample;
            offline_sample.timestamp_ms = now_ms;
            offline_sample.offline = true;
            history.samples.push_back(std::move(offline_sample));
            is_dirty = true;
        }
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

    const QString primary_path = ResolveHistoryFilePath();
    QFile primary_file(primary_path);
    const bool primary_exists = primary_file.exists();
    if (primary_exists && !primary_file.open(QIODevice::ReadOnly)) {
        return;
    }

    QByteArray payload;
    if (primary_exists) {
        payload = primary_file.readAll();
        primary_file.close();
    }

    const QString backup_path = ResolveHistoryBackupFilePath(primary_path);
    QFile backup_file(backup_path);
    const bool backup_exists = backup_file.exists() && backup_file.size() > 0;

    auto parse_payload = [this](const QByteArray& raw, QHash<QString, BatteryHistoryData>* out) {
        out->clear();
        const auto document = QJsonDocument::fromJson(raw);
        if (!document.isObject()) {
            return false;
        }

        const auto root_object = document.object();
        const auto devices_value = root_object.value(QStringLiteral("devices"));
        if (!devices_value.isArray()) {
            return false;
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
                out->insert(device_id, std::move(history));
            }
        }
        return true;
    };

    if (primary_exists && parse_payload(payload, &history_by_device_)) {
        return;
    }

    // The primary file is missing or corrupted: when present, preserve it for
    // diagnosis and fall back to the backup copy if one exists; otherwise
    // reset to a diagnosable empty store.
    if (primary_exists) {
        const QString corrupt_path = ResolveHistoryCorruptFilePath(primary_path);
        QFile::rename(primary_path, corrupt_path);
        qWarning() << "Battery history file was corrupted; moved to" << corrupt_path;
    }

    if (backup_exists) {
        if (backup_file.open(QIODevice::ReadOnly)) {
            const QByteArray backup_payload = backup_file.readAll();
            backup_file.close();
            if (parse_payload(backup_payload, &history_by_device_)) {
                return;
            }
        }
        QFile::rename(backup_path, ResolveHistoryCorruptFilePath(backup_path));
        qWarning() << "Battery history backup was corrupted; moved next to the primary file.";
    }
}

void BatteryHistoryStore::SaveToDisk() const {
    const QString primary_path = ResolveHistoryFilePath();

    QByteArray payload = QJsonDocument(SerializeHistory(history_by_device_))
                             .toJson(QJsonDocument::Indented);
    if (payload.size() > kMaxHistoryFileBytes) {
        // Too large: drop samples older than half the retention window and
        // retry once. If it still does not fit, skip the write with a
        // diagnostic rather than persisting an oversized file.
        const qint64 cutoff_ms =
            QDateTime::currentMSecsSinceEpoch() - kHistoryRetentionMs / 2;
        QHash<QString, BatteryHistoryData> trimmed;
        for (auto it = history_by_device_.cbegin(); it != history_by_device_.cend(); ++it) {
            BatteryHistoryData history = it.value();
            while (!history.samples.isEmpty() && history.samples.front().timestamp_ms < cutoff_ms) {
                history.samples.removeFirst();
            }
            trimmed.insert(it.key(), std::move(history));
        }
        payload = QJsonDocument(SerializeHistory(trimmed)).toJson(QJsonDocument::Indented);
        if (payload.size() > kMaxHistoryFileBytes) {
            qWarning() << "Battery history exceeds the size limit; skipping save.";
            return;
        }
    }

    // Refresh the backup copy before replacing the primary file (atomic
    // temp+rename write via QSaveFile) so a corrupted primary can be recovered.
    const QString backup_path = ResolveHistoryBackupFilePath(primary_path);
    if (QFile::exists(primary_path)) {
        QFile::remove(backup_path);
        if (!QFile::copy(primary_path, backup_path)) {
            qWarning() << "Failed to refresh the battery history backup copy.";
        }
    }

    QSaveFile file(primary_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }

    file.write(payload);
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

    if (history->samples.size() > kMaxSamplesPerDevice) {
        DownsampleHistory(history, QDateTime::currentMSecsSinceEpoch());
    }
}

}  // namespace battery_monitor
