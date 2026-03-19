#include "ui/BatteryWindowSettings.h"

#include <algorithm>
#include <unordered_set>

#include <QSettings>
#include <QString>
#include <QStringList>

namespace battery_monitor {

namespace {

constexpr const char* kSettingsGroupUi = "ui";
constexpr const char* kSettingsConnectedOrderKey = "connected_order";
constexpr const char* kSettingsDisconnectedOrderKey = "disconnected_order";
constexpr const char* kSettingsRefreshIntervalMsKey = "refresh_interval_ms";
constexpr const char* kSettingsLowBatteryThresholdPercentKey = "low_battery_threshold_percent";
constexpr const char* kSettingsLowBatteryRepeatMinutesKey = "low_battery_repeat_minutes";

QSettings CreateUiSettings() {
    return QSettings(
        QSettings::IniFormat,
        QSettings::UserScope,
        QStringLiteral("BatteryMonitor"),
        QStringLiteral("BatteryMonitor"));
}

std::vector<std::string> ReadOrderFromSettings(QSettings* settings, const char* key) {
    if (settings == nullptr || key == nullptr) {
        return {};
    }

    const QStringList saved_values = settings->value(QString::fromLatin1(key)).toStringList();
    std::vector<std::string> order;
    order.reserve(static_cast<std::size_t>(saved_values.size()));
    std::unordered_set<std::string> seen;
    for (const auto& value : saved_values) {
        const std::string device_id = value.toUtf8().toStdString();
        if (device_id.empty() || !seen.emplace(device_id).second) {
            continue;
        }
        order.push_back(device_id);
    }
    return order;
}

void WriteOrderToSettings(QSettings* settings, const char* key, const std::vector<std::string>& order) {
    if (settings == nullptr || key == nullptr) {
        return;
    }

    QStringList values;
    values.reserve(static_cast<qsizetype>(order.size()));
    for (const auto& device_id : order) {
        if (device_id.empty()) {
            continue;
        }
        values.push_back(QString::fromUtf8(device_id.c_str()));
    }
    settings->setValue(QString::fromLatin1(key), values);
}

}  // namespace

int ClampBatteryWindowRefreshIntervalMs(int interval_ms) {
    const int min_ms = kBatteryWindowMinRefreshIntervalSeconds * 1000;
    const int max_ms = kBatteryWindowMaxRefreshIntervalSeconds * 1000;
    return std::clamp(interval_ms, min_ms, max_ms);
}

int ClampBatteryWindowLowBatteryThresholdPercent(int percent) {
    return std::clamp(
        percent, kBatteryWindowMinLowBatteryThresholdPercent, kBatteryWindowMaxLowBatteryThresholdPercent);
}

int ClampBatteryWindowLowBatteryRepeatMinutes(int minutes) {
    return std::clamp(
        minutes, kBatteryWindowMinLowBatteryRepeatMinutes, kBatteryWindowMaxLowBatteryRepeatMinutes);
}

BatteryWindowPersistedState LoadBatteryWindowPersistedState() {
    BatteryWindowPersistedState state;
    QSettings settings = CreateUiSettings();
    settings.beginGroup(QString::fromLatin1(kSettingsGroupUi));
    state.refresh_interval_ms = ClampBatteryWindowRefreshIntervalMs(
        settings.value(
                    QString::fromLatin1(kSettingsRefreshIntervalMsKey),
                    kBatteryWindowDefaultRefreshIntervalMs)
            .toInt());
    state.low_battery_threshold_percent = ClampBatteryWindowLowBatteryThresholdPercent(
        settings.value(
                    QString::fromLatin1(kSettingsLowBatteryThresholdPercentKey),
                    kBatteryWindowDefaultLowBatteryThresholdPercent)
            .toInt());
    state.low_battery_repeat_minutes = ClampBatteryWindowLowBatteryRepeatMinutes(
        settings.value(
                    QString::fromLatin1(kSettingsLowBatteryRepeatMinutesKey),
                    kBatteryWindowDefaultLowBatteryRepeatMinutes)
            .toInt());
    state.connected_device_order = ReadOrderFromSettings(&settings, kSettingsConnectedOrderKey);
    state.disconnected_device_order = ReadOrderFromSettings(&settings, kSettingsDisconnectedOrderKey);
    settings.endGroup();
    return state;
}

void SaveBatteryWindowRefreshIntervalMs(int interval_ms) {
    QSettings settings = CreateUiSettings();
    settings.beginGroup(QString::fromLatin1(kSettingsGroupUi));
    settings.setValue(
        QString::fromLatin1(kSettingsRefreshIntervalMsKey),
        ClampBatteryWindowRefreshIntervalMs(interval_ms));
    settings.endGroup();
    settings.sync();
}

void SaveBatteryWindowLowBatteryThresholdPercent(int percent) {
    QSettings settings = CreateUiSettings();
    settings.beginGroup(QString::fromLatin1(kSettingsGroupUi));
    settings.setValue(
        QString::fromLatin1(kSettingsLowBatteryThresholdPercentKey),
        ClampBatteryWindowLowBatteryThresholdPercent(percent));
    settings.endGroup();
    settings.sync();
}

void SaveBatteryWindowLowBatteryRepeatMinutes(int minutes) {
    QSettings settings = CreateUiSettings();
    settings.beginGroup(QString::fromLatin1(kSettingsGroupUi));
    settings.setValue(
        QString::fromLatin1(kSettingsLowBatteryRepeatMinutesKey),
        ClampBatteryWindowLowBatteryRepeatMinutes(minutes));
    settings.endGroup();
    settings.sync();
}

void SaveBatteryWindowDeviceOrder(const std::vector<std::string>& connected_order,
                                  const std::vector<std::string>& disconnected_order) {
    QSettings settings = CreateUiSettings();
    settings.beginGroup(QString::fromLatin1(kSettingsGroupUi));
    WriteOrderToSettings(&settings, kSettingsConnectedOrderKey, connected_order);
    WriteOrderToSettings(&settings, kSettingsDisconnectedOrderKey, disconnected_order);
    settings.endGroup();
    settings.sync();
}

}  // namespace battery_monitor
