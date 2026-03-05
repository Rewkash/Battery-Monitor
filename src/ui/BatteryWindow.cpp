#include "ui/BatteryWindow.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QDataStream>
#include <QDateTime>
#include <QDebug>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPropertyAnimation>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QSettings>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#ifdef _WIN32
#include <winrt/base.h>
#endif

namespace battery_monitor {

namespace {

struct ComponentEntry {
    std::string component;
    std::optional<std::uint8_t> battery_level_percent;
    bool is_cached = false;
    bool is_connected = true;
};

struct DeviceEntry {
    std::string device_id;
    std::string device_name;
    std::vector<ComponentEntry> components;
    bool is_connected = false;
};

struct PrimaryBattery {
    std::optional<std::uint8_t> level;
    bool cached = false;
};

struct SummaryCounts {
    int devices = 0;
    int components = 0;
    int live = 0;
    int cached = 0;
    int no_data = 0;
};

struct RefreshTaskResult {
    std::vector<DeviceBatteryInfo> devices;
    QString error_text;
    bool is_bluetooth_stack_error = false;
};

constexpr int kMaxVisibleRows = 3;
constexpr int kCollapsedRowHeight = 88;
constexpr int kListPadding = 12;
constexpr int kListSpacing = 10;
constexpr int kListHeightSlack = 28;
constexpr const char* kDeviceRowMimeType = "application/x-chargeview-device-row";
constexpr const char* kSettingsGroupUi = "ui";
constexpr const char* kSettingsConnectedOrderKey = "connected_order";
constexpr const char* kSettingsDisconnectedOrderKey = "disconnected_order";
constexpr const char* kSettingsRefreshIntervalMsKey = "refresh_interval_ms";
constexpr const char* kSettingsLowBatteryThresholdPercentKey = "low_battery_threshold_percent";
constexpr const char* kSettingsLowBatteryRepeatMinutesKey = "low_battery_repeat_minutes";
constexpr int kDefaultRefreshIntervalMs = 15000;
constexpr int kMinRefreshIntervalSeconds = 5;
constexpr int kMaxRefreshIntervalSeconds = 600;
constexpr int kDefaultLowBatteryThresholdPercent = 10;
constexpr int kMinLowBatteryThresholdPercent = 1;
constexpr int kMaxLowBatteryThresholdPercent = 100;
constexpr int kDefaultLowBatteryRepeatMinutes = 10;
constexpr int kMinLowBatteryRepeatMinutes = 1;
constexpr int kMaxLowBatteryRepeatMinutes = 180;

QSettings CreateUiSettings() {
    return QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("BatteryMonitor"),
                     QStringLiteral("BatteryMonitor"));
}

int ClampRefreshIntervalMs(int interval_ms) {
    const int min_ms = kMinRefreshIntervalSeconds * 1000;
    const int max_ms = kMaxRefreshIntervalSeconds * 1000;
    return std::clamp(interval_ms, min_ms, max_ms);
}

int ClampLowBatteryThresholdPercent(int percent) {
    return std::clamp(percent, kMinLowBatteryThresholdPercent, kMaxLowBatteryThresholdPercent);
}

int ClampLowBatteryRepeatMinutes(int minutes) {
    return std::clamp(minutes, kMinLowBatteryRepeatMinutes, kMaxLowBatteryRepeatMinutes);
}

int LoadRefreshIntervalMs() {
    QSettings settings = CreateUiSettings();
    settings.beginGroup(QString::fromLatin1(kSettingsGroupUi));
    const int saved = settings.value(QString::fromLatin1(kSettingsRefreshIntervalMsKey), kDefaultRefreshIntervalMs)
                          .toInt();
    settings.endGroup();
    return ClampRefreshIntervalMs(saved);
}

void SaveRefreshIntervalMs(int interval_ms) {
    QSettings settings = CreateUiSettings();
    settings.beginGroup(QString::fromLatin1(kSettingsGroupUi));
    settings.setValue(QString::fromLatin1(kSettingsRefreshIntervalMsKey), ClampRefreshIntervalMs(interval_ms));
    settings.endGroup();
    settings.sync();
}

int LoadLowBatteryThresholdPercent() {
    QSettings settings = CreateUiSettings();
    settings.beginGroup(QString::fromLatin1(kSettingsGroupUi));
    const int saved = settings
                          .value(QString::fromLatin1(kSettingsLowBatteryThresholdPercentKey),
                                 kDefaultLowBatteryThresholdPercent)
                          .toInt();
    settings.endGroup();
    return ClampLowBatteryThresholdPercent(saved);
}

void SaveLowBatteryThresholdPercent(int percent) {
    QSettings settings = CreateUiSettings();
    settings.beginGroup(QString::fromLatin1(kSettingsGroupUi));
    settings.setValue(QString::fromLatin1(kSettingsLowBatteryThresholdPercentKey),
                      ClampLowBatteryThresholdPercent(percent));
    settings.endGroup();
    settings.sync();
}

int LoadLowBatteryRepeatMinutes() {
    QSettings settings = CreateUiSettings();
    settings.beginGroup(QString::fromLatin1(kSettingsGroupUi));
    const int saved = settings
                          .value(QString::fromLatin1(kSettingsLowBatteryRepeatMinutesKey),
                                 kDefaultLowBatteryRepeatMinutes)
                          .toInt();
    settings.endGroup();
    return ClampLowBatteryRepeatMinutes(saved);
}

void SaveLowBatteryRepeatMinutes(int minutes) {
    QSettings settings = CreateUiSettings();
    settings.beginGroup(QString::fromLatin1(kSettingsGroupUi));
    settings.setValue(QString::fromLatin1(kSettingsLowBatteryRepeatMinutesKey),
                      ClampLowBatteryRepeatMinutes(minutes));
    settings.endGroup();
    settings.sync();
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

void LoadPersistedDeviceOrder(std::vector<std::string>* connected_order,
                              std::vector<std::string>* disconnected_order) {
    QSettings settings = CreateUiSettings();
    settings.beginGroup(QString::fromLatin1(kSettingsGroupUi));
    if (connected_order != nullptr) {
        *connected_order = ReadOrderFromSettings(&settings, kSettingsConnectedOrderKey);
    }
    if (disconnected_order != nullptr) {
        *disconnected_order = ReadOrderFromSettings(&settings, kSettingsDisconnectedOrderKey);
    }
    settings.endGroup();
}

void SavePersistedDeviceOrder(const std::vector<std::string>& connected_order,
                              const std::vector<std::string>& disconnected_order) {
    QSettings settings = CreateUiSettings();
    settings.beginGroup(QString::fromLatin1(kSettingsGroupUi));
    WriteOrderToSettings(&settings, kSettingsConnectedOrderKey, connected_order);
    WriteOrderToSettings(&settings, kSettingsDisconnectedOrderKey, disconnected_order);
    settings.endGroup();
    settings.sync();
}

class DraggableDeviceRow final : public QFrame {
   public:
    using ReorderCallback = std::function<void(const std::string& dragged_device_id,
                                               const std::string& target_device_id,
                                               bool connected_queue,
                                               bool insert_before_target)>;
    using DragStateCallback = std::function<void(bool active)>;

    DraggableDeviceRow(std::string device_id, bool is_connected, QWidget* parent = nullptr)
        : QFrame(parent), device_id_(std::move(device_id)), is_connected_(is_connected) {
        setAcceptDrops(true);
        setCursor(Qt::OpenHandCursor);
        setProperty("dragOver", false);
    }

    void SetReorderCallback(ReorderCallback callback) {
        reorder_callback_ = std::move(callback);
    }

    void SetDragStateCallback(DragStateCallback callback) {
        drag_state_callback_ = std::move(callback);
    }

   protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event != nullptr && event->button() == Qt::LeftButton) {
            drag_start_pos_ = event->position().toPoint();
            setCursor(Qt::ClosedHandCursor);
        }
        QFrame::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (event == nullptr || !(event->buttons() & Qt::LeftButton)) {
            QFrame::mouseMoveEvent(event);
            return;
        }
        if ((event->position().toPoint() - drag_start_pos_).manhattanLength() < QApplication::startDragDistance()) {
            QFrame::mouseMoveEvent(event);
            return;
        }

        StartDrag();
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        setCursor(Qt::OpenHandCursor);
        QFrame::mouseReleaseEvent(event);
    }

    void dragEnterEvent(QDragEnterEvent* event) override {
        if (CanAccept(event != nullptr ? event->mimeData() : nullptr)) {
            event->acceptProposedAction();
            SetDragOver(true);
            return;
        }
        if (event != nullptr) {
            event->ignore();
        }
    }

    void dragMoveEvent(QDragMoveEvent* event) override {
        if (CanAccept(event != nullptr ? event->mimeData() : nullptr)) {
            event->acceptProposedAction();
            return;
        }
        if (event != nullptr) {
            event->ignore();
        }
    }

    void dragLeaveEvent(QDragLeaveEvent* event) override {
        Q_UNUSED(event);
        SetDragOver(false);
    }

    void dropEvent(QDropEvent* event) override {
        std::string dragged_device_id;
        bool dragged_connected = false;
        if (event == nullptr ||
            !DecodePayload(event->mimeData(), &dragged_device_id, &dragged_connected) ||
            dragged_device_id == device_id_ || dragged_connected != is_connected_) {
            if (event != nullptr) {
                event->ignore();
            }
            SetDragOver(false);
            return;
        }

        SetDragOver(false);
        const bool insert_before_target = event->position().y() < (static_cast<double>(height()) / 2.0);
        if (reorder_callback_) {
            reorder_callback_(dragged_device_id, device_id_, is_connected_, insert_before_target);
        }
        event->acceptProposedAction();
    }

   private:
    static QByteArray BuildPayload(const std::string& device_id, bool is_connected) {
        QByteArray payload;
        QDataStream stream(&payload, QIODevice::WriteOnly);
        stream << QString::fromUtf8(device_id.c_str()) << is_connected;
        return payload;
    }

    static bool DecodePayload(const QMimeData* mime_data, std::string* device_id, bool* is_connected) {
        if (mime_data == nullptr || device_id == nullptr || is_connected == nullptr ||
            !mime_data->hasFormat(kDeviceRowMimeType)) {
            return false;
        }

        QByteArray payload = mime_data->data(kDeviceRowMimeType);
        QDataStream stream(&payload, QIODevice::ReadOnly);
        QString decoded_id;
        bool decoded_connected = false;
        stream >> decoded_id >> decoded_connected;
        if (stream.status() != QDataStream::Ok || decoded_id.isEmpty()) {
            return false;
        }

        *device_id = decoded_id.toUtf8().toStdString();
        *is_connected = decoded_connected;
        return true;
    }

    bool CanAccept(const QMimeData* mime_data) const {
        std::string dragged_device_id;
        bool dragged_connected = false;
        return DecodePayload(mime_data, &dragged_device_id, &dragged_connected) &&
               dragged_connected == is_connected_ &&
               dragged_device_id != device_id_;
    }

    void SetDragOver(bool enabled) {
        if (property("dragOver").toBool() == enabled) {
            return;
        }
        setProperty("dragOver", enabled);
        if (auto* widget_style = style(); widget_style != nullptr) {
            widget_style->unpolish(this);
            widget_style->polish(this);
        }
        update();
    }

    void StartDrag() {
        auto* drag = new QDrag(this);
        auto* mime_data = new QMimeData();
        mime_data->setData(kDeviceRowMimeType, BuildPayload(device_id_, is_connected_));
        drag->setMimeData(mime_data);
        drag->setHotSpot(rect().center());
        if (drag_state_callback_) {
            drag_state_callback_(true);
        }
        drag->exec(Qt::MoveAction);
        if (drag_state_callback_) {
            drag_state_callback_(false);
        }
        setCursor(Qt::OpenHandCursor);
    }

    std::string device_id_;
    bool is_connected_ = false;
    QPoint drag_start_pos_;
    ReorderCallback reorder_callback_;
    DragStateCallback drag_state_callback_;
};

QString ToQString(const std::string& value) {
    return QString::fromUtf8(value.c_str());
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string NormalizeComponentName(const std::string& component) {
    if (component.empty()) {
        return "main";
    }
    return ToLowerAscii(component);
}

bool IsDisconnectedEarbudLevel(const std::string& component, const std::optional<std::uint8_t>& level) {
    if (!level.has_value()) {
        return false;
    }
    return (component == "left" || component == "right") && *level == 0;
}

int ComponentRank(const std::string& component) {
    if (component == "left") {
        return 0;
    }
    if (component == "right") {
        return 1;
    }
    if (component == "case") {
        return 2;
    }
    if (component == "main") {
        return 3;
    }
    return 10;
}

int ComponentQualityScore(const ComponentEntry& entry) {
    int score = 0;
    if (entry.battery_level_percent.has_value()) {
        score += 1000;
        score += static_cast<int>(*entry.battery_level_percent);
    }
    if (!entry.is_cached) {
        score += 100;
    }
    return score;
}

const ComponentEntry* FindComponent(const DeviceEntry& device, const char* name) {
    const auto it = std::find_if(device.components.begin(), device.components.end(),
                                 [name](const ComponentEntry& component) {
                                     return component.component == name;
                                 });
    return it == device.components.end() ? nullptr : &(*it);
}

bool HasLiveData(const DeviceEntry& device) {
    return std::any_of(device.components.begin(), device.components.end(), [](const ComponentEntry& component) {
        return component.battery_level_percent.has_value() && !component.is_cached;
    });
}

bool IsDeviceConnected(const DeviceEntry& device) {
    return device.is_connected;
}

bool HasAnyData(const DeviceEntry& device) {
    return std::any_of(device.components.begin(), device.components.end(), [](const ComponentEntry& component) {
        return component.battery_level_percent.has_value();
    });
}

PrimaryBattery ComputeAveragePrimary(const std::vector<const ComponentEntry*>& components) {
    PrimaryBattery result;
    if (components.empty()) {
        return result;
    }

    int sum = 0;
    bool all_cached = true;
    for (const auto* component : components) {
        sum += static_cast<int>(*component->battery_level_percent);
        all_cached = all_cached && component->is_cached;
    }

    const int rounded_average = (sum + static_cast<int>(components.size()) / 2) /
                                static_cast<int>(components.size());
    result.level = static_cast<std::uint8_t>(std::clamp(rounded_average, 0, 100));
    result.cached = all_cached;
    return result;
}

PrimaryBattery ComputePrimaryBattery(const DeviceEntry& device) {
    PrimaryBattery result;

    const auto* left = FindComponent(device, "left");
    const auto* right = FindComponent(device, "right");
    const auto* case_level = FindComponent(device, "case");

    const bool has_left = left != nullptr && left->battery_level_percent.has_value();
    const bool has_right = right != nullptr && right->battery_level_percent.has_value();
    const bool has_case = case_level != nullptr && case_level->battery_level_percent.has_value();

    // For TWS, primary level should represent earbuds. Ignore case when both earbuds are available.
    if (has_left && has_right) {
        return ComputeAveragePrimary({left, right});
    }

    if (const auto* main = FindComponent(device, "main"); main != nullptr && main->battery_level_percent.has_value()) {
        result.level = main->battery_level_percent;
        result.cached = main->is_cached;
        return result;
    }

    if (has_left) {
        result.level = left->battery_level_percent;
        result.cached = left->is_cached;
        return result;
    }

    if (has_right) {
        result.level = right->battery_level_percent;
        result.cached = right->is_cached;
        return result;
    }

    if (has_case) {
        result.level = case_level->battery_level_percent;
        result.cached = case_level->is_cached;
        return result;
    }

    std::vector<const ComponentEntry*> candidates;
    if (candidates.empty()) {
        for (const auto& component : device.components) {
            if (component.battery_level_percent.has_value()) {
                candidates.push_back(&component);
            }
        }
    }

    if (candidates.empty()) {
        return result;
    }

    return ComputeAveragePrimary(candidates);
}

QString FormatOptionalPercent(const std::optional<std::uint8_t>& value) {
    if (!value.has_value()) {
        return QStringLiteral("--");
    }
    return QString::number(*value);
}

QString BuildComponentTriplet(const DeviceEntry& device) {
    const auto* left = FindComponent(device, "left");
    const auto* right = FindComponent(device, "right");
    const auto* case_level = FindComponent(device, "case");

    if (left == nullptr && right == nullptr && case_level == nullptr) {
        return {};
    }

    const auto left_text = FormatOptionalPercent(left != nullptr ? left->battery_level_percent : std::nullopt);
    const auto right_text = FormatOptionalPercent(right != nullptr ? right->battery_level_percent : std::nullopt);
    const auto case_text = FormatOptionalPercent(case_level != nullptr ? case_level->battery_level_percent : std::nullopt);

    return QString::fromUtf8(u8"\u041B:%1 \u041F:%2 \u041A:%3").arg(left_text, right_text, case_text);
}

QString ProgressLevelState(const PrimaryBattery& primary) {
    if (!primary.level.has_value()) {
        return QStringLiteral("na");
    }

    const int level = static_cast<int>(*primary.level);
    if (level < 20) {
        return QStringLiteral("low");
    }
    if (level < 40) {
        return QStringLiteral("warn");
    }
    return QStringLiteral("ok");
}

QString DeviceTypeCode(const DeviceEntry& device) {
    const std::string probe = ToLowerAscii(device.device_name + " " + device.device_id);

    if (probe.find("bud") != std::string::npos || probe.find("airpod") != std::string::npos ||
        probe.find("ear") != std::string::npos || probe.find("head") != std::string::npos) {
        return QStringLiteral("HP");
    }
    if (probe.find("keyboard") != std::string::npos || probe.find("klav") != std::string::npos) {
        return QStringLiteral("KB");
    }
    if (probe.find("mouse") != std::string::npos || probe.find("mice") != std::string::npos) {
        return QStringLiteral("MS");
    }
    if (probe.find("xbox") != std::string::npos || probe.find("controller") != std::string::npos ||
        probe.find("gamepad") != std::string::npos) {
        return QStringLiteral("PAD");
    }
    if (probe.find("phone") != std::string::npos || probe.find("poco") != std::string::npos ||
        probe.find("redmi") != std::string::npos) {
        return QStringLiteral("PH");
    }

    return QStringLiteral("BT");
}

QString RelativeTimeText(const QDateTime& from_time, const QDateTime& to_time) {
    if (!from_time.isValid()) {
        return QString::fromUtf8(u8"\u043D\u0435\u0442 \u0434\u0430\u043D\u043D\u044B\u0445 \u043E "
                                 u8"\u0432\u0440\u0435\u043C\u0435\u043D\u0438 \u043E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u0438\u044F");
    }

    qint64 delta_seconds_64 = from_time.secsTo(to_time);
    if (delta_seconds_64 < 0) {
        delta_seconds_64 = 0;
    }
    const int delta_seconds = static_cast<int>(delta_seconds_64);
    if (delta_seconds < 10) {
        return QString::fromUtf8(u8"\u043E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u043E "
                                 u8"\u0442\u043E\u043B\u044C\u043A\u043E \u0447\u0442\u043E");
    }
    if (delta_seconds < 60) {
        return QString::fromUtf8(u8"\u043E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u043E %1 \u0441 "
                                 u8"\u043D\u0430\u0437\u0430\u0434")
            .arg(delta_seconds);
    }

    const int delta_minutes = delta_seconds / 60;
    if (delta_minutes < 60) {
        return QString::fromUtf8(u8"\u043E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u043E %1 \u043C\u0438\u043D "
                                 u8"\u043D\u0430\u0437\u0430\u0434")
            .arg(delta_minutes);
    }

    const int delta_hours = delta_minutes / 60;
    if (delta_hours < 24) {
        return QString::fromUtf8(u8"\u043E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u043E %1 \u0447 "
                                 u8"\u043D\u0430\u0437\u0430\u0434")
            .arg(delta_hours);
    }

    const int delta_days = delta_hours / 24;
    return QString::fromUtf8(u8"\u043E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u043E %1 \u0434 "
                             u8"\u043D\u0430\u0437\u0430\u0434")
        .arg(delta_days);
}

QString DeviceStatusText(const DeviceEntry& device,
                         const std::unordered_map<std::string, QDateTime>& last_live_update,
                         const QDateTime& now) {
    if (!IsDeviceConnected(device)) {
        const auto it = last_live_update.find(device.device_id);
        if (it != last_live_update.end()) {
            return QString::fromUtf8(u8"\u041D\u0435 \u043F\u043E\u0434\u043A\u043B\u044E\u0447\u0435\u043D\u043E \u00B7 ") +
                   RelativeTimeText(it->second, now);
        }
        return QString::fromUtf8(u8"\u041D\u0435 \u043F\u043E\u0434\u043A\u043B\u044E\u0447\u0435\u043D\u043E");
    }

    if (HasLiveData(device)) {
        const auto it = last_live_update.find(device.device_id);
        if (it != last_live_update.end()) {
            return RelativeTimeText(it->second, now);
        }
        return QString::fromUtf8(u8"\u043E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u043E "
                                 u8"\u0442\u043E\u043B\u044C\u043A\u043E \u0447\u0442\u043E");
    }

    if (HasAnyData(device)) {
        const auto it = last_live_update.find(device.device_id);
        if (it != last_live_update.end()) {
            return QString::fromUtf8(u8"\u041F\u043E\u0434\u043A\u043B\u044E\u0447\u0435\u043D\u043E \u00B7 ") +
                   RelativeTimeText(it->second, now) +
                   QString::fromUtf8(u8" (\u043A\u044D\u0448)");
        }
        return QString::fromUtf8(u8"\u041F\u043E\u0434\u043A\u043B\u044E\u0447\u0435\u043D\u043E \u00B7 "
                                 u8"\u041A\u044D\u0448\u0438\u0440\u043E\u0432\u0430\u043D\u043D\u043E\u0435 "
                                 u8"\u0437\u043D\u0430\u0447\u0435\u043D\u0438\u0435");
    }

    return QString::fromUtf8(u8"\u041F\u043E\u0434\u043A\u043B\u044E\u0447\u0435\u043D\u043E \u00B7 "
                             u8"\u041D\u0435\u0442 \u0434\u0430\u043D\u043D\u044B\u0445 \u043E "
                             u8"\u0437\u0430\u0440\u044F\u0434\u0435");
}

SummaryCounts ComputeSummaryCounts(const std::vector<DeviceEntry>& devices) {
    SummaryCounts counts;
    counts.devices = static_cast<int>(devices.size());

    for (const auto& device : devices) {
        for (const auto& component : device.components) {
            ++counts.components;
            if (!component.battery_level_percent.has_value()) {
                ++counts.no_data;
                continue;
            }
            if (component.is_cached) {
                ++counts.cached;
            } else {
                ++counts.live;
            }
        }
    }

    return counts;
}

QString BuildSummaryLine(const SummaryCounts& counts, int hidden_count, const QDateTime& now) {
    return QString::fromUtf8(u8"%1  \u0423\u0441\u0442\u0440:%2  \u041A\u043E\u043C\u043F\u043E\u043D\u0435\u043D\u0442\u044B:%3  "
                             u8"Live:%4  \u041A\u044D\u0448:%5  \u041D/\u0414:%6  "
                             u8"\u0421\u043A\u0440\u044B\u0442\u043E:%7")
        .arg(now.toString(QStringLiteral("HH:mm:ss")))
        .arg(counts.devices)
        .arg(counts.components)
        .arg(counts.live)
        .arg(counts.cached)
        .arg(counts.no_data)
        .arg(hidden_count);
}

std::vector<DeviceEntry> GroupDevices(const std::vector<DeviceBatteryInfo>& devices,
                                      const std::unordered_set<std::string>& hidden_device_ids) {
    std::vector<DeviceEntry> grouped;
    std::unordered_map<std::string, std::size_t> index_by_id;

    for (const auto& item : devices) {
        const std::string device_id = item.device_id.empty() ? "UnknownDevice" : item.device_id;
        if (hidden_device_ids.contains(device_id)) {
            continue;
        }

        const std::string device_name = item.device_name.empty() ? "Unknown device" : item.device_name;

        std::size_t index = 0;
        const auto found = index_by_id.find(device_id);
        if (found == index_by_id.end()) {
            index = grouped.size();
            index_by_id.emplace(device_id, index);

            DeviceEntry device;
            device.device_id = device_id;
            device.device_name = device_name;
            device.is_connected = item.is_connected;
            grouped.push_back(std::move(device));
        } else {
            index = found->second;
            if (grouped[index].device_name == "Unknown device" && device_name != "Unknown device") {
                grouped[index].device_name = device_name;
            }
            grouped[index].is_connected = grouped[index].is_connected || item.is_connected;
        }

        ComponentEntry incoming;
        incoming.component = NormalizeComponentName(item.battery_component);
        incoming.battery_level_percent = item.battery_level_percent;
        incoming.is_cached = item.is_cached;
        incoming.is_connected = item.is_connected;
        if (IsDisconnectedEarbudLevel(incoming.component, incoming.battery_level_percent)) {
            incoming.battery_level_percent = std::nullopt;
        }

        auto& target = grouped[index].components;
        const auto existing = std::find_if(target.begin(), target.end(), [&incoming](const ComponentEntry& current) {
            return current.component == incoming.component;
        });

        if (existing == target.end()) {
            target.push_back(std::move(incoming));
        } else if (ComponentQualityScore(incoming) > ComponentQualityScore(*existing)) {
            *existing = std::move(incoming);
        }
    }

    for (auto& device : grouped) {
        std::sort(device.components.begin(), device.components.end(),
                  [](const ComponentEntry& lhs, const ComponentEntry& rhs) {
                      const int lhs_rank = ComponentRank(lhs.component);
                      const int rhs_rank = ComponentRank(rhs.component);
                      if (lhs_rank != rhs_rank) {
                          return lhs_rank < rhs_rank;
                      }
                      return lhs.component < rhs.component;
                  });
    }

    std::sort(grouped.begin(), grouped.end(), [](const DeviceEntry& lhs, const DeviceEntry& rhs) {
        const bool lhs_active = IsDeviceConnected(lhs);
        const bool rhs_active = IsDeviceConnected(rhs);
        if (lhs_active != rhs_active) {
            return lhs_active > rhs_active;
        }

        const auto lhs_name = ToLowerAscii(lhs.device_name);
        const auto rhs_name = ToLowerAscii(rhs.device_name);
        if (lhs_name != rhs_name) {
            return lhs_name < rhs_name;
        }
        return lhs.device_id < rhs.device_id;
    });

    return grouped;
}

void SyncOrderQueue(const std::vector<DeviceEntry>& grouped, bool connected_queue, std::vector<std::string>* order) {
    if (order == nullptr) {
        return;
    }

    std::unordered_set<std::string> present_ids;
    for (const auto& device : grouped) {
        if (device.is_connected == connected_queue) {
            present_ids.insert(device.device_id);
        }
    }

    order->erase(std::remove_if(order->begin(), order->end(),
                                [&present_ids](const std::string& device_id) {
                                    return !present_ids.contains(device_id);
                                }),
                 order->end());

    std::unordered_set<std::string> ordered_ids(order->begin(), order->end());
    for (const auto& device : grouped) {
        if (device.is_connected == connected_queue && !ordered_ids.contains(device.device_id)) {
            order->push_back(device.device_id);
            ordered_ids.insert(device.device_id);
        }
    }
}

std::vector<DeviceEntry> ApplyCustomOrder(const std::vector<DeviceEntry>& grouped,
                                          const std::vector<std::string>& connected_order,
                                          const std::vector<std::string>& disconnected_order) {
    std::vector<DeviceEntry> ordered;
    ordered.reserve(grouped.size());

    std::unordered_map<std::string, const DeviceEntry*> by_id;
    by_id.reserve(grouped.size());
    for (const auto& device : grouped) {
        by_id.emplace(device.device_id, &device);
    }

    std::unordered_set<std::string> emitted;
    emitted.reserve(grouped.size());

    auto append_queue = [&](const std::vector<std::string>& queue, bool connected_queue) {
        for (const auto& device_id : queue) {
            const auto it = by_id.find(device_id);
            if (it == by_id.end()) {
                continue;
            }
            const DeviceEntry* device = it->second;
            if (device == nullptr || device->is_connected != connected_queue || emitted.contains(device->device_id)) {
                continue;
            }
            ordered.push_back(*device);
            emitted.insert(device->device_id);
        }
    };

    append_queue(connected_order, true);
    append_queue(disconnected_order, false);

    for (const auto& device : grouped) {
        if (emitted.contains(device.device_id)) {
            continue;
        }
        ordered.push_back(device);
        emitted.insert(device.device_id);
    }

    return ordered;
}

bool ReorderQueueItems(std::vector<std::string>* order,
                       const std::string& dragged_device_id,
                       const std::string& target_device_id,
                       bool insert_before_target) {
    if (order == nullptr || dragged_device_id.empty() || target_device_id.empty() ||
        dragged_device_id == target_device_id) {
        return false;
    }

    const auto dragged_it = std::find(order->begin(), order->end(), dragged_device_id);
    const auto target_it = std::find(order->begin(), order->end(), target_device_id);
    if (dragged_it == order->end() || target_it == order->end()) {
        return false;
    }

    const auto dragged_index = static_cast<std::ptrdiff_t>(std::distance(order->begin(), dragged_it));
    const auto target_index = static_cast<std::ptrdiff_t>(std::distance(order->begin(), target_it));
    const auto requested_index = insert_before_target ? target_index : target_index + 1;
    const auto normalized_index = (dragged_index < requested_index) ? requested_index - 1 : requested_index;
    if (dragged_index == normalized_index) {
        return false;
    }

    const std::string moved_id = *dragged_it;
    order->erase(dragged_it);
    const auto insert_position = order->begin() + normalized_index;
    order->insert(insert_position, moved_id);
    return true;
}

struct LowBatteryComponentState {
    std::string component;
    std::uint8_t level = 0;
    bool below_threshold = false;
    bool triggered = false;
};

struct LowBatteryDeviceState {
    QString device_name;
    std::vector<LowBatteryComponentState> components;
};

struct LowBatteryNotificationText {
    QString title;
    QString line1;
    QString line2;
    std::uint8_t critical_level = 100;
};

int ComponentShortRank(const std::string& component_name) {
    if (component_name == "left") {
        return 0;
    }
    if (component_name == "right") {
        return 1;
    }
    if (component_name == "case") {
        return 2;
    }
    if (component_name == "main") {
        return 3;
    }
    return 10;
}

QString ComponentDisplayLabel(const std::string& component_name) {
    if (component_name == "left") {
        return QString::fromUtf8(u8"\u041B\u0435\u0432\u044B\u0439");
    }
    if (component_name == "right") {
        return QString::fromUtf8(u8"\u041F\u0440\u0430\u0432\u044B\u0439");
    }
    if (component_name == "case") {
        return QString::fromUtf8(u8"\u041A\u0435\u0439\u0441");
    }
    return QString::fromUtf8(u8"\u0417\u0430\u0440\u044F\u0434");
}

LowBatteryNotificationText FormatLowBatteryNotification(const LowBatteryDeviceState& device_state) {
    LowBatteryNotificationText result;
    result.title = device_state.device_name.isEmpty()
                       ? QString::fromUtf8(u8"\u0423\u0441\u0442\u0440\u043E\u0439\u0441\u0442\u0432\u043E")
                       : device_state.device_name;
    if (device_state.components.empty()) {
        return result;
    }

    std::vector<std::size_t> below_indices;
    for (std::size_t i = 0; i < device_state.components.size(); ++i) {
        if (device_state.components[i].below_threshold) {
            below_indices.push_back(i);
        }
    }

    const auto choose_more_critical = [&](std::size_t lhs, std::size_t rhs) {
        const auto& a = device_state.components[lhs];
        const auto& b = device_state.components[rhs];
        if (a.level != b.level) {
            return a.level < b.level;
        }
        return ComponentShortRank(a.component) < ComponentShortRank(b.component);
    };

    std::size_t primary_index = 0;
    if (!below_indices.empty()) {
        primary_index = below_indices.front();
        for (std::size_t i = 1; i < below_indices.size(); ++i) {
            if (choose_more_critical(below_indices[i], primary_index)) {
                primary_index = below_indices[i];
            }
        }
    } else {
        for (std::size_t i = 1; i < device_state.components.size(); ++i) {
            if (choose_more_critical(i, primary_index)) {
                primary_index = i;
            }
        }
    }

    const auto& primary = device_state.components[primary_index];
    const QString primary_label = ComponentDisplayLabel(primary.component);
    result.critical_level = primary.level;
    result.line1 = QString::fromUtf8(u8"%1: %2% \u2014 \u0437\u0430\u0440\u044F\u0434\u0438\u0442\u0435")
                       .arg(primary_label)
                       .arg(static_cast<int>(primary.level));

    std::vector<std::size_t> other_indices;
    for (std::size_t i = 0; i < device_state.components.size(); ++i) {
        if (i == primary_index) {
            continue;
        }
        other_indices.push_back(i);
    }
    std::sort(other_indices.begin(), other_indices.end(), [&](std::size_t lhs, std::size_t rhs) {
        const auto& a = device_state.components[lhs];
        const auto& b = device_state.components[rhs];
        const int rank_a = ComponentShortRank(a.component);
        const int rank_b = ComponentShortRank(b.component);
        if (rank_a != rank_b) {
            return rank_a < rank_b;
        }
        return a.level < b.level;
    });

    QStringList other_values;
    other_values.reserve(static_cast<qsizetype>(other_indices.size()));
    for (const std::size_t i : other_indices) {
        const auto& component = device_state.components[i];
        other_values.push_back(QString::fromUtf8(u8"%1: %2%")
                                   .arg(ComponentDisplayLabel(component.component))
                                   .arg(static_cast<int>(component.level)));
    }
    result.line2 = other_values.join(QString::fromUtf8(u8" \u00B7 "));
    return result;
}

void RunLowBatteryNotificationSelfCheck() {
#ifndef NDEBUG
    static const bool ran_once = []() {
        auto assert_match = [](const LowBatteryDeviceState& input,
                               const QString& exp_title,
                               const QString& exp_line1,
                               const QString& exp_line2) {
            const auto out = FormatLowBatteryNotification(input);
            if (out.title != exp_title || out.line1 != exp_line1 || out.line2 != exp_line2) {
                qWarning().noquote()
                    << "LowBattery format mismatch:"
                    << "title='" << out.title << "'"
                    << "line1='" << out.line1 << "'"
                    << "line2='" << out.line2 << "'";
            }
        };

        assert_match(
            LowBatteryDeviceState{
                QString::fromUtf8(u8"Redmi Buds 4 Pro"),
                {LowBatteryComponentState{"right", 19, true, true},
                 LowBatteryComponentState{"left", 90, false, false},
                 LowBatteryComponentState{"case", 39, false, false}}},
            QString::fromUtf8(u8"Redmi Buds 4 Pro"),
            QString::fromUtf8(u8"\u041F\u0440\u0430\u0432\u044B\u0439: 19% \u2014 \u0437\u0430\u0440\u044F\u0434\u0438\u0442\u0435"),
            QString::fromUtf8(u8"\u041B\u0435\u0432\u044B\u0439: 90% \u00B7 \u041A\u0435\u0439\u0441: 39%"));

        assert_match(
            LowBatteryDeviceState{
                QString::fromUtf8(u8"Redmi Buds 4 Pro"),
                {LowBatteryComponentState{"left", 15, true, true}}},
            QString::fromUtf8(u8"Redmi Buds 4 Pro"),
            QString::fromUtf8(u8"\u041B\u0435\u0432\u044B\u0439: 15% \u2014 \u0437\u0430\u0440\u044F\u0434\u0438\u0442\u0435"),
            QString());

        assert_match(
            LowBatteryDeviceState{
                QString::fromUtf8(u8"Redmi Buds 4 Pro"),
                {LowBatteryComponentState{"case", 8, true, true}}},
            QString::fromUtf8(u8"Redmi Buds 4 Pro"),
            QString::fromUtf8(u8"\u041A\u0435\u0439\u0441: 8% \u2014 \u0437\u0430\u0440\u044F\u0434\u0438\u0442\u0435"),
            QString());

        assert_match(
            LowBatteryDeviceState{
                QString::fromUtf8(u8"POCO F3"),
                {LowBatteryComponentState{"main", 22, true, true}}},
            QString::fromUtf8(u8"POCO F3"),
            QString::fromUtf8(u8"\u0417\u0430\u0440\u044F\u0434: 22% \u2014 \u0437\u0430\u0440\u044F\u0434\u0438\u0442\u0435"),
            QString());
        return true;
    }();
    Q_UNUSED(ran_once);
#endif
}

class LowBatteryToast final : public QFrame {
   public:
    LowBatteryToast()
        : QFrame(nullptr),
          title_label_(new QLabel(this)),
          line1_dot_label_(new QLabel(this)),
          line1_label_(new QLabel(this)),
          line2_label_(new QLabel(this)),
          slide_in_animation_(new QPropertyAnimation(this, "pos", this)),
          slide_out_animation_(new QPropertyAnimation(this, "pos", this)),
          hide_timer_(new QTimer(this)) {
        setObjectName(QStringLiteral("lowBatteryToast"));
        setWindowFlag(Qt::Tool, true);
        setWindowFlag(Qt::FramelessWindowHint, true);
        setWindowFlag(Qt::WindowStaysOnTopHint, true);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setAttribute(Qt::WA_TranslucentBackground, false);
        setAttribute(Qt::WA_StyledBackground, true);
        setFocusPolicy(Qt::NoFocus);
        setWindowOpacity(1.0);

        setStyleSheet(R"(
QFrame#lowBatteryToast {
    background: #3B3E44;
    border: 1px solid rgba(255, 255, 255, 0.09);
    border-radius: 12px;
}
QLabel#toastTitle {
    color: #F8FAFC;
    font-size: 15px;
    font-weight: 700;
}
QLabel#toastLine1Dot {
    color: #FF5A5A;
    font-size: 14px;
    font-weight: 800;
}
QLabel#toastLine1 {
    color: #F8FAFC;
    font-size: 14px;
    font-weight: 700;
}
QLabel#toastLine2 {
    color: #D1D5DB;
    font-size: 11px;
    font-weight: 500;
}
)");

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 10, 12, 10);
        layout->setSpacing(4);

        title_label_->setObjectName(QStringLiteral("toastTitle"));
        title_label_->setTextInteractionFlags(Qt::NoTextInteraction);
        title_label_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        auto* line1_layout = new QHBoxLayout();
        line1_layout->setContentsMargins(0, 0, 0, 0);
        line1_layout->setSpacing(6);

        line1_dot_label_->setObjectName(QStringLiteral("toastLine1Dot"));
        line1_dot_label_->setText(QString::fromUtf8(u8"\u25CF"));
        line1_dot_label_->setFixedWidth(10);
        line1_dot_label_->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);

        line1_label_->setObjectName(QStringLiteral("toastLine1"));
        line1_label_->setWordWrap(true);
        line1_label_->setTextInteractionFlags(Qt::NoTextInteraction);

        line2_label_->setObjectName(QStringLiteral("toastLine2"));
        line2_label_->setWordWrap(true);
        line2_label_->setTextInteractionFlags(Qt::NoTextInteraction);

        line1_layout->addWidget(line1_dot_label_);
        line1_layout->addWidget(line1_label_, 1);

        layout->addWidget(title_label_);
        layout->addLayout(line1_layout);
        layout->addWidget(line2_label_);

        hide_timer_->setSingleShot(true);

        slide_in_animation_->setDuration(240);
        slide_in_animation_->setEasingCurve(QEasingCurve::OutCubic);
        slide_out_animation_->setDuration(220);
        slide_out_animation_->setEasingCurve(QEasingCurve::InCubic);

        connect(slide_in_animation_, &QPropertyAnimation::finished, this, [this]() {
            if (isVisible() && visible_duration_ms_ > 0) {
                hide_timer_->start(visible_duration_ms_);
            }
        });
        connect(hide_timer_, &QTimer::timeout, this, [this]() { StartHideAnimation(); });
        connect(slide_out_animation_, &QPropertyAnimation::finished, this, [this]() { hide(); });
    }

    void ShowNotification(const QString& title,
                          const QString& line1,
                          const QString& line2,
                          int visible_ms) {
        title_label_->setText(title);
        line1_label_->setText(line1);
        line2_label_->setText(line2);
        line2_label_->setVisible(!line2.trimmed().isEmpty());
        visible_duration_ms_ = std::max(1000, visible_ms);

        hide_timer_->stop();
        slide_in_animation_->stop();
        slide_out_animation_->stop();

        UpdateGeometryForCurrentScreen();
        move(hidden_pos_);
        show();
        raise();

        slide_in_animation_->setStartValue(hidden_pos_);
        slide_in_animation_->setEndValue(visible_pos_);
        slide_in_animation_->start();
    }

   private:
    void StartHideAnimation() {
        if (!isVisible()) {
            return;
        }
        slide_out_animation_->stop();
        slide_out_animation_->setStartValue(pos());
        slide_out_animation_->setEndValue(hidden_pos_);
        slide_out_animation_->start();
    }

    void UpdateGeometryForCurrentScreen() {
        QScreen* screen = QGuiApplication::primaryScreen();
        QRect available = screen != nullptr ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
        const int margin = 16;
        const int toast_width = std::clamp(available.width() - (margin * 2), 260, 360);
        setFixedWidth(toast_width);
        adjustSize();
        setFixedHeight(sizeHint().height());

        const int x_visible = available.right() - this->width() - margin;
        const int y_visible = available.top() + margin;
        visible_pos_ = QPoint(x_visible, y_visible);
        hidden_pos_ = QPoint(available.right() + margin, y_visible);
    }

    QLabel* title_label_ = nullptr;
    QLabel* line1_dot_label_ = nullptr;
    QLabel* line1_label_ = nullptr;
    QLabel* line2_label_ = nullptr;
    QPropertyAnimation* slide_in_animation_ = nullptr;
    QPropertyAnimation* slide_out_animation_ = nullptr;
    QTimer* hide_timer_ = nullptr;
    QPoint visible_pos_;
    QPoint hidden_pos_;
    int visible_duration_ms_ = 7000;
};

void ShowLowBatteryToastMessage(const QString& title,
                                const QString& line1,
                                const QString& line2,
                                int visible_ms) {
    static QPointer<LowBatteryToast> toast;
    if (toast.isNull()) {
        toast = new LowBatteryToast();
    }
    toast->ShowNotification(title, line1, line2, visible_ms);
}

#ifdef _WIN32
QString FormatWinRtError(const winrt::hresult_error& error) {
    std::ostringstream stream;
    stream << "WinRT HRESULT=0x" << std::uppercase << std::hex
           << static_cast<std::uint32_t>(error.code().value) << std::dec;
    const auto message = winrt::to_string(error.message());
    if (!message.empty()) {
        stream << " (" << message << ")";
    }
    return ToQString(stream.str());
}
#endif

}  // namespace

BatteryWindow::BatteryWindow(std::unique_ptr<IBluetoothBatteryProvider> provider, QWidget* parent)
    : QWidget(parent), provider_(std::move(provider)) {
    setObjectName(QStringLiteral("trayPanelWindow"));
    setWindowTitle(QStringLiteral("ChargeView"));
    setWindowFlag(Qt::Tool, true);
    setWindowFlag(Qt::WindowStaysOnTopHint, true);
    setWindowFlag(Qt::FramelessWindowHint, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAutoFillBackground(false);
    setFixedWidth(390);

    setStyleSheet(R"(
QWidget#trayPanelWindow {
    background: transparent;
    color: #F7F7F7;
    border: none;
}
QLabel#titleLabel {
    font-size: 15px;
    font-weight: 700;
    color: #F8FAFC;
}
QPushButton#topButton {
    background: #44484F;
    color: #F8FAFC;
    border: 1px solid rgba(255, 255, 255, 0.14);
    border-radius: 10px;
    padding: 4px 10px;
    font-size: 11px;
    font-weight: 600;
}
QPushButton#topButton:hover {
    background: #50555E;
}
QPushButton#topButton:disabled {
    color: #9CA3AF;
    background: #373A40;
    border-color: rgba(255, 255, 255, 0.08);
}
QLabel#summaryLabel {
    color: #D1D5DB;
    font-size: 10px;
}
QFrame#deviceRow {
    background: #3B3E44;
    border: 1px solid rgba(255, 255, 255, 0.09);
    border-radius: 14px;
}
QFrame#deviceRow[activeState="inactive"] {
    background: #32363D;
    border-color: rgba(255, 255, 255, 0.06);
}
QFrame#deviceRow:hover {
    border-color: rgba(255, 255, 255, 0.16);
}
QFrame#deviceRow[dragOver="true"] {
    border-color: rgba(116, 190, 255, 0.85);
}
QLabel#deviceIcon {
    background: #2B2F36;
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 11px;
    color: #E8ECF3;
    font-size: 11px;
    font-weight: 700;
}
QLabel#deviceName {
    color: #F7FAFD;
    font-size: 15px;
    font-weight: 600;
}
QLabel#deviceName[activeState="inactive"] {
    color: #C5CCD8;
}
QLabel#technicalMeta {
    color: #8892A2;
    font-size: 11px;
    font-weight: 400;
}
QLabel#technicalMeta[activeState="inactive"] {
    color: #7F8795;
}
QLabel#percentChip {
    background: #2A2F37;
    color: #E7ECF6;
    border: 1px solid rgba(255, 255, 255, 0.14);
    border-radius: 9px;
    padding: 1px 7px;
    font-size: 12px;
    font-weight: 700;
}
QLabel#percentChip[levelState="low"] {
    background: #41242A;
    border-color: #8E404A;
    color: #FFD0D5;
}
QLabel#percentChip[activeState="inactive"] {
    background: #262B33;
    border-color: rgba(255, 255, 255, 0.10);
    color: #B7BFCD;
}
QProgressBar#deviceProgress {
    background: #171A20;
    border: 1px solid rgba(255, 255, 255, 0.08);
    border-radius: 6px;
    min-height: 10px;
    max-height: 10px;
}
QProgressBar#deviceProgress::chunk {
    border-radius: 5px;
    background: #30C26E;
}
QProgressBar#deviceProgress[levelState="ok"]::chunk {
    background: #30C26E;
}
QProgressBar#deviceProgress[levelState="warn"]::chunk {
    background: #D7B446;
}
QProgressBar#deviceProgress[levelState="low"]::chunk {
    background: #E06767;
}
QProgressBar#deviceProgress[levelState="na"]::chunk {
    background: #5F6876;
}
QProgressBar#deviceProgress[activeState="inactive"] {
    border-color: rgba(255, 255, 255, 0.05);
}
QProgressBar#deviceProgress[activeState="inactive"]::chunk {
    background: #5F6876;
}
QToolButton#inlineMenuButton {
    background: transparent;
    color: #A7B0C0;
    border: none;
    min-width: 28px;
    max-width: 28px;
    min-height: 28px;
    max-height: 28px;
    border-radius: 8px;
    font-size: 15px;
    font-weight: 600;
}
QToolButton#inlineMenuButton:hover {
    background: rgba(255, 255, 255, 0.08);
    color: #D6DCE7;
}
QToolButton#settingsButton {
    background: #40444B;
    color: #E7EDF8;
    border: 1px solid rgba(255, 255, 255, 0.14);
    border-radius: 10px;
    min-width: 30px;
    max-width: 30px;
    min-height: 30px;
    max-height: 30px;
    font-size: 14px;
    font-weight: 700;
}
QToolButton#settingsButton:hover {
    background: #4D525B;
}
QFrame#settingsPanel {
    background: #3A3E45;
    border: 1px solid rgba(255, 255, 255, 0.10);
    border-radius: 10px;
}
QLabel#settingsLabel {
    color: #D8DEE9;
    font-size: 11px;
    font-weight: 500;
}
QSpinBox#settingsSpinBox {
    background: #2E3238;
    color: #F2F5FB;
    border: 1px solid rgba(255, 255, 255, 0.14);
    border-radius: 8px;
    padding: 2px 6px;
    min-height: 24px;
    min-width: 90px;
    font-size: 11px;
}
QLabel#statusLabel {
    color: #B6BDCA;
    font-size: 11px;
}
QLabel#emptyLabel {
    color: #E5E7EB;
    font-size: 12px;
}
QScrollArea {
    background: transparent;
    border: none;
}
QScrollArea > QWidget > QWidget {
    background: transparent;
}
QWidget#listContainer {
    background: transparent;
}
)");

    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(8, 8, 8, 8);
    root_layout->setSpacing(6);

    auto* top_layout = new QHBoxLayout();
    top_layout->setContentsMargins(10, 0, 10, 0);
    top_layout->setSpacing(6);

    auto* title_label = new QLabel(QStringLiteral("ChargeView"), this);
    title_label->setObjectName(QStringLiteral("titleLabel"));

    refresh_button_ = new QPushButton(QString::fromUtf8(u8"\u041E\u0431\u043D\u043E\u0432\u0438\u0442\u044C"), this);
    refresh_button_->setObjectName(QStringLiteral("topButton"));

    show_all_button_ = new QPushButton(QString::fromUtf8(u8"\u041F\u043E\u043A\u0430\u0437\u0430\u0442\u044C "
                                                         u8"\u0441\u043A\u0440\u044B\u0442\u044B\u0435"), this);
    show_all_button_->setObjectName(QStringLiteral("topButton"));
    show_all_button_->setEnabled(false);

    top_layout->addWidget(title_label, 1);
    top_layout->addWidget(show_all_button_);
    top_layout->addWidget(refresh_button_);

    summary_label_ = new QLabel(QStringLiteral("--"), this);
    summary_label_->setObjectName(QStringLiteral("summaryLabel"));
    summary_label_->setVisible(false);

    scroll_area_ = new QScrollArea(this);
    scroll_area_->setWidgetResizable(true);
    scroll_area_->setFrameShape(QFrame::NoFrame);
    scroll_area_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_area_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_area_->viewport()->setAutoFillBackground(false);
    scroll_area_->viewport()->setAttribute(Qt::WA_StyledBackground, false);

    cards_container_ = new QWidget(scroll_area_);
    cards_container_->setObjectName(QStringLiteral("listContainer"));
    cards_layout_ = new QVBoxLayout(cards_container_);
    cards_layout_->setContentsMargins(kListPadding, kListPadding, kListPadding, kListPadding);
    cards_layout_->setSpacing(kListSpacing);

    scroll_area_->setWidget(cards_container_);

    settings_panel_ = new QFrame(this);
    settings_panel_->setObjectName(QStringLiteral("settingsPanel"));
    settings_panel_->setVisible(false);
    settings_panel_->setMinimumHeight(0);
    settings_panel_->setMaximumHeight(0);
    settings_panel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* settings_layout = new QVBoxLayout(settings_panel_);
    settings_layout->setContentsMargins(10, 7, 10, 7);
    settings_layout->setSpacing(6);

    auto* refresh_row_layout = new QHBoxLayout();
    refresh_row_layout->setContentsMargins(0, 0, 0, 0);
    refresh_row_layout->setSpacing(8);

    auto* refresh_settings_label =
        new QLabel(QString::fromUtf8(u8"\u0410\u0432\u0442\u043E\u043E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u0438\u0435 (сек):"),
                   settings_panel_);
    refresh_settings_label->setObjectName(QStringLiteral("settingsLabel"));

    refresh_interval_spinbox_ = new QSpinBox(settings_panel_);
    refresh_interval_spinbox_->setObjectName(QStringLiteral("settingsSpinBox"));
    refresh_interval_spinbox_->setRange(kMinRefreshIntervalSeconds, kMaxRefreshIntervalSeconds);
    refresh_interval_spinbox_->setSingleStep(5);
    refresh_interval_spinbox_->setSuffix(QString::fromUtf8(u8" с"));

    auto* threshold_row_layout = new QHBoxLayout();
    threshold_row_layout->setContentsMargins(0, 0, 0, 0);
    threshold_row_layout->setSpacing(8);

    auto* threshold_settings_label = new QLabel(
        QString::fromUtf8(u8"\u041F\u043E\u0440\u043E\u0433 \u0443\u0432\u0435\u0434\u043E\u043C\u043B\u0435\u043D\u0438\u044F (%):"),
        settings_panel_);
    threshold_settings_label->setObjectName(QStringLiteral("settingsLabel"));

    low_battery_threshold_spinbox_ = new QSpinBox(settings_panel_);
    low_battery_threshold_spinbox_->setObjectName(QStringLiteral("settingsSpinBox"));
    low_battery_threshold_spinbox_->setRange(kMinLowBatteryThresholdPercent, kMaxLowBatteryThresholdPercent);
    low_battery_threshold_spinbox_->setSingleStep(1);
    low_battery_threshold_spinbox_->setSuffix(QStringLiteral("%"));

    auto* repeat_row_layout = new QHBoxLayout();
    repeat_row_layout->setContentsMargins(0, 0, 0, 0);
    repeat_row_layout->setSpacing(8);

    auto* repeat_settings_label = new QLabel(
        QString::fromUtf8(u8"\u041F\u043E\u0432\u0442\u043E\u0440 \u0443\u0432\u0435\u0434\u043E\u043C\u043B\u0435\u043D\u0438\u044F (мин):"),
        settings_panel_);
    repeat_settings_label->setObjectName(QStringLiteral("settingsLabel"));

    low_battery_repeat_spinbox_ = new QSpinBox(settings_panel_);
    low_battery_repeat_spinbox_->setObjectName(QStringLiteral("settingsSpinBox"));
    low_battery_repeat_spinbox_->setRange(kMinLowBatteryRepeatMinutes, kMaxLowBatteryRepeatMinutes);
    low_battery_repeat_spinbox_->setSingleStep(1);
    low_battery_repeat_spinbox_->setSuffix(QString::fromUtf8(u8" мин"));

    refresh_row_layout->addWidget(refresh_settings_label);
    refresh_row_layout->addStretch(1);
    refresh_row_layout->addWidget(refresh_interval_spinbox_);

    threshold_row_layout->addWidget(threshold_settings_label);
    threshold_row_layout->addStretch(1);
    threshold_row_layout->addWidget(low_battery_threshold_spinbox_);

    repeat_row_layout->addWidget(repeat_settings_label);
    repeat_row_layout->addStretch(1);
    repeat_row_layout->addWidget(low_battery_repeat_spinbox_);

    settings_layout->addLayout(refresh_row_layout);
    settings_layout->addLayout(threshold_row_layout);
    settings_layout->addLayout(repeat_row_layout);

    settings_panel_animation_ = new QPropertyAnimation(settings_panel_, "maximumHeight", this);
    settings_panel_animation_->setDuration(200);
    settings_panel_animation_->setEasingCurve(QEasingCurve::OutCubic);
    connect(settings_panel_animation_, &QPropertyAnimation::valueChanged, this,
            [this](const QVariant&) { AdjustWindowHeightForRows(kMaxVisibleRows); });
    connect(settings_panel_animation_, &QPropertyAnimation::finished, this, [this]() {
        if (settings_panel_ != nullptr && !settings_panel_expanded_) {
            settings_panel_->setVisible(false);
        }
        AdjustWindowHeightForRows(kMaxVisibleRows);
    });

    status_label_ = new QLabel(QString::fromUtf8(u8"\u0413\u043E\u0442\u043E\u0432\u043E"), this);
    status_label_->setObjectName(QStringLiteral("statusLabel"));
    status_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status_label_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    settings_button_ = new QToolButton(this);
    settings_button_->setObjectName(QStringLiteral("settingsButton"));
    settings_button_->setText(QString::fromUtf8(u8"\u2699"));
    settings_button_->setCursor(Qt::PointingHandCursor);
    settings_button_->setToolButtonStyle(Qt::ToolButtonTextOnly);

    auto* footer_layout = new QHBoxLayout();
    footer_layout->setContentsMargins(10, 0, 10, 0);
    footer_layout->setSpacing(6);
    footer_layout->addWidget(status_label_, 1);
    footer_layout->addStretch(1);
    footer_layout->addWidget(settings_button_);

    root_layout->addLayout(top_layout);
    root_layout->addWidget(summary_label_);
    root_layout->addWidget(scroll_area_, 1);
    root_layout->addWidget(settings_panel_);
    root_layout->addLayout(footer_layout);

    connect(refresh_button_, &QPushButton::clicked, this, [this]() { RefreshBatteryData(); });
    connect(show_all_button_, &QPushButton::clicked, this, [this]() { ResetHiddenDevices(); });
    connect(settings_button_, &QToolButton::clicked, this, [this]() { ConfigureRefreshInterval(); });
    connect(refresh_interval_spinbox_,
            static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, [this](int seconds) { ApplyRefreshIntervalSeconds(seconds, true); });
    connect(low_battery_threshold_spinbox_,
            static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, [this](int percent) { ApplyLowBatteryThresholdPercent(percent, true); });
    connect(low_battery_repeat_spinbox_,
            static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, [this](int minutes) { ApplyLowBatteryRepeatMinutes(minutes, true); });

    refresh_timer_ = new QTimer(this);
    refresh_interval_ms_ = LoadRefreshIntervalMs();
    low_battery_threshold_percent_ = LoadLowBatteryThresholdPercent();
    low_battery_repeat_minutes_ = LoadLowBatteryRepeatMinutes();
    refresh_timer_->setInterval(refresh_interval_ms_);
    if (refresh_interval_spinbox_ != nullptr) {
        refresh_interval_spinbox_->blockSignals(true);
        refresh_interval_spinbox_->setValue(std::max(kMinRefreshIntervalSeconds, refresh_interval_ms_ / 1000));
        refresh_interval_spinbox_->blockSignals(false);
    }
    if (low_battery_threshold_spinbox_ != nullptr) {
        low_battery_threshold_spinbox_->blockSignals(true);
        low_battery_threshold_spinbox_->setValue(low_battery_threshold_percent_);
        low_battery_threshold_spinbox_->blockSignals(false);
    }
    if (low_battery_repeat_spinbox_ != nullptr) {
        low_battery_repeat_spinbox_->blockSignals(true);
        low_battery_repeat_spinbox_->setValue(low_battery_repeat_minutes_);
        low_battery_repeat_spinbox_->blockSignals(false);
    }
    connect(refresh_timer_, &QTimer::timeout, this, [this]() { RefreshBatteryData(); });
    refresh_timer_->start();
    UpdateRefreshSettingsTooltip();
    RunLowBatteryNotificationSelfCheck();

    LoadPersistedDeviceOrder(&connected_device_order_, &disconnected_device_order_);

    AdjustWindowHeightForRows(kMaxVisibleRows);
    InitializeTray();
    RefreshBatteryData();
}

BatteryWindow::~BatteryWindow() {
    quitting_ = true;
    if (refresh_worker_.joinable()) {
        if (refresh_worker_.get_id() == std::this_thread::get_id()) {
            refresh_worker_.detach();
        } else {
            refresh_worker_.join();
        }
    }
}

void BatteryWindow::Launch() {
    if (tray_icon_ != nullptr && tray_icon_->isVisible()) {
        hide();
        return;
    }

    show();
    raise();
    activateWindow();
}

void BatteryWindow::closeEvent(QCloseEvent* event) {
    if (tray_icon_ != nullptr && tray_icon_->isVisible() && !quitting_) {
        event->ignore();
        HideWindowToTray();
        return;
    }

    QWidget::closeEvent(event);
}

bool BatteryWindow::event(QEvent* event) {
    if (event != nullptr && event->type() == QEvent::WindowDeactivate) {
        if (tray_icon_ != nullptr && tray_icon_->isVisible() && isVisible() && !quitting_) {
            if (tray_menu_ != nullptr && tray_menu_->isVisible()) {
                return QWidget::event(event);
            }
            HideWindowToTray();
            return true;
        }
    }

    return QWidget::event(event);
}

void BatteryWindow::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QRectF panel_rect = rect();
    panel_rect.adjust(0.5, 0.5, -0.5, -0.5);

    QPainterPath path;
    path.addRoundedRect(panel_rect, 24.0, 24.0);

    painter.fillPath(path, QColor(QStringLiteral("#35363A")));
    painter.setPen(QPen(QColor(QStringLiteral("#575C65")), 1.0));
    painter.drawPath(path);
}

void BatteryWindow::InitializeTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        status_label_->setText(QString::fromUtf8(
            u8"\u0421\u0438\u0441\u0442\u0435\u043C\u043D\u044B\u0439 \u0442\u0440\u0435\u0439 "
            u8"\u043D\u0435\u0434\u043E\u0441\u0442\u0443\u043F\u0435\u043D. \u041E\u043A\u043D\u043E "
            u8"\u0440\u0430\u0431\u043E\u0442\u0430\u0435\u0442 \u0432 \u043E\u0431\u044B\u0447\u043D\u043E\u043C "
            u8"\u0440\u0435\u0436\u0438\u043C\u0435."));
        return;
    }

    if (qApp != nullptr) {
        qApp->setQuitOnLastWindowClosed(false);
    }

    QIcon icon = QIcon::fromTheme(QStringLiteral("bluetooth"));
    if (icon.isNull()) {
        icon = style()->standardIcon(QStyle::SP_ComputerIcon);
    }

    setWindowIcon(icon);

    tray_icon_ = new QSystemTrayIcon(icon, this);
    tray_menu_ = new QMenu(this);

    toggle_window_action_ = tray_menu_->addAction(QString::fromUtf8(u8"\u0421\u043A\u0440\u044B\u0442\u044C \u043E\u043A\u043D\u043E"));
    refresh_action_ = tray_menu_->addAction(QString::fromUtf8(u8"\u041E\u0431\u043D\u043E\u0432\u0438\u0442\u044C \u0441\u0435\u0439\u0447\u0430\u0441"));
    reset_hidden_action_ = tray_menu_->addAction(QString::fromUtf8(
        u8"\u041F\u043E\u043A\u0430\u0437\u0430\u0442\u044C \u0441\u043A\u0440\u044B\u0442\u044B\u0435 "
        u8"\u0443\u0441\u0442\u0440\u043E\u0439\u0441\u0442\u0432\u0430"));
    tray_menu_->addSeparator();
    quit_action_ = tray_menu_->addAction(QString::fromUtf8(u8"\u0412\u044B\u0445\u043E\u0434"));

    connect(toggle_window_action_, &QAction::triggered, this, [this]() {
        if (isVisible()) {
            HideWindowToTray();
        } else {
            ShowWindowFromTray();
        }
    });
    connect(refresh_action_, &QAction::triggered, this, [this]() { RefreshBatteryData(); });
    connect(reset_hidden_action_, &QAction::triggered, this, [this]() { ResetHiddenDevices(); });
    connect(quit_action_, &QAction::triggered, this, [this]() { QuitFromTray(); });

    connect(tray_icon_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
                    if (isVisible()) {
                        HideWindowToTray();
                    } else {
                        ShowWindowFromTray();
                    }
                }
            });

    tray_icon_->setContextMenu(tray_menu_);
    tray_icon_->setToolTip(QStringLiteral("ChargeView"));
    tray_icon_->show();

    UpdateToggleActionText();
}

void BatteryWindow::ShowWindowFromTray() {
    const QPoint cursor_position = QCursor::pos();
    QScreen* screen = QGuiApplication::screenAt(cursor_position);
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen != nullptr) {
        const QRect area = screen->availableGeometry();
        const int margin = 8;
        int target_x = cursor_position.x() - width() + 24;
        int target_y = cursor_position.y() - height() - 24;
        target_x = std::clamp(target_x, area.left() + margin, area.right() - width() - margin);
        target_y = std::clamp(target_y, area.top() + margin, area.bottom() - height() - margin);
        move(target_x, target_y);
    }

    show();
    setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    raise();
    activateWindow();
    UpdateToggleActionText();
}

void BatteryWindow::HideWindowToTray() {
    settings_panel_expanded_ = false;
    if (settings_panel_animation_ != nullptr) {
        settings_panel_animation_->stop();
    }
    if (settings_panel_ != nullptr) {
        settings_panel_->setMaximumHeight(0);
        settings_panel_->setMinimumHeight(0);
        settings_panel_->setVisible(false);
    }
    AdjustWindowHeightForRows(kMaxVisibleRows);

    hide();
    UpdateToggleActionText();
}

void BatteryWindow::QuitFromTray() {
    quitting_ = true;
    if (tray_icon_ != nullptr) {
        tray_icon_->hide();
    }
    close();
    if (qApp != nullptr) {
        qApp->quit();
    }
}

void BatteryWindow::ResetHiddenDevices() {
    hidden_device_ids_.clear();
    RefreshBatteryData();
}

void BatteryWindow::UpdateToggleActionText() {
    if (toggle_window_action_ == nullptr) {
        return;
    }
    toggle_window_action_->setText(
        isVisible() ? QString::fromUtf8(u8"\u0421\u043A\u0440\u044B\u0442\u044C \u043E\u043A\u043D\u043E")
                    : QString::fromUtf8(u8"\u041F\u043E\u043A\u0430\u0437\u0430\u0442\u044C \u043E\u043A\u043D\u043E"));
}

void BatteryWindow::UpdateTrayTooltip(const std::vector<DeviceBatteryInfo>& devices) {
    if (tray_icon_ == nullptr) {
        return;
    }

    const auto grouped = GroupDevices(devices, hidden_device_ids_);
    const auto ordered = ApplyCustomOrder(grouped, connected_device_order_, disconnected_device_order_);
    QString tooltip = QString::fromUtf8(u8"ChargeView\n"
                                       u8"\u0423\u0441\u0442\u0440\u043E\u0439\u0441\u0442\u0432: %1")
                          .arg(ordered.size());

    int shown = 0;
    for (const auto& device : ordered) {
        if (shown >= 4) {
            break;
        }

        const auto primary = ComputePrimaryBattery(device);
        const QString level_text = primary.level.has_value()
                                       ? QStringLiteral("%1%").arg(*primary.level)
                                       : QString::fromUtf8(u8"\u041D/\u0414");
        tooltip += QStringLiteral("\n%1: %2").arg(ToQString(device.device_name), level_text);
        ++shown;
    }

    tray_icon_->setToolTip(tooltip);
}

void BatteryWindow::NotifyLowBatteryIfNeeded(const std::vector<DeviceBatteryInfo>& devices) {
    std::unordered_map<std::string, LowBatteryDeviceState> device_states;
    const std::uint8_t threshold_percent =
        static_cast<std::uint8_t>(ClampLowBatteryThresholdPercent(low_battery_threshold_percent_));
    const std::int64_t repeat_interval_ms =
        static_cast<std::int64_t>(ClampLowBatteryRepeatMinutes(low_battery_repeat_minutes_)) * 60LL * 1000LL;
    const std::int64_t now_ms = QDateTime::currentMSecsSinceEpoch();

    for (const auto& entry : devices) {
        const std::string component = NormalizeComponentName(entry.battery_component);
        const std::string key = entry.device_id + "|" + component;
        if (!entry.is_connected) {
            last_live_component_levels_.erase(key);
            last_low_battery_alert_ms_.erase(key);
            continue;
        }
        if (entry.is_cached || !entry.battery_level_percent.has_value()) {
            continue;
        }
        const std::uint8_t current_level = *entry.battery_level_percent;
        const auto previous_it = last_live_component_levels_.find(key);
        const bool is_below_threshold = current_level < threshold_percent;
        bool should_alert = false;
        if (is_below_threshold) {
            if (previous_it == last_live_component_levels_.end() || previous_it->second >= threshold_percent) {
                should_alert = true;
            } else {
                const auto alert_it = last_low_battery_alert_ms_.find(key);
                should_alert = (alert_it == last_low_battery_alert_ms_.end()) ||
                               (now_ms - alert_it->second >= repeat_interval_ms);
            }
        } else {
            last_low_battery_alert_ms_.erase(key);
        }
        last_live_component_levels_[key] = current_level;
        if (!should_alert) {
            // keep component state for contextual line2 formatting
        } else {
            last_low_battery_alert_ms_[key] = now_ms;
        }

        auto& device_state = device_states[entry.device_id];
        if (device_state.device_name.isEmpty()) {
            device_state.device_name = ToQString(entry.device_name);
        }

        auto component_it = std::find_if(device_state.components.begin(),
                                         device_state.components.end(),
                                         [&](const LowBatteryComponentState& state) {
                                             return state.component == component;
                                         });
        if (component_it == device_state.components.end()) {
            device_state.components.push_back(
                LowBatteryComponentState{component, current_level, is_below_threshold, should_alert});
        } else {
            component_it->level = current_level;
            component_it->below_threshold = is_below_threshold;
            component_it->triggered = component_it->triggered || should_alert;
        }
    }

    std::vector<LowBatteryNotificationText> notifications;
    notifications.reserve(device_states.size());
    for (const auto& [device_id, state] : device_states) {
        Q_UNUSED(device_id);
        const bool has_triggered = std::any_of(state.components.begin(), state.components.end(),
                                               [](const LowBatteryComponentState& component) {
                                                   return component.triggered;
                                               });
        if (!has_triggered) {
            continue;
        }
        notifications.push_back(FormatLowBatteryNotification(state));
    }

    if (notifications.empty()) {
        return;
    }

    std::sort(notifications.begin(), notifications.end(),
              [](const LowBatteryNotificationText& lhs, const LowBatteryNotificationText& rhs) {
                  return lhs.critical_level < rhs.critical_level;
              });

    QApplication::beep();

    const auto& most_critical = notifications.front();
    ShowLowBatteryToastMessage(most_critical.title, most_critical.line1, most_critical.line2, 7000);

    if (status_label_ != nullptr) {
        status_label_->setText(most_critical.title + QStringLiteral(": ") + most_critical.line1);
    }
}
void BatteryWindow::AdjustWindowHeightForRows(int visible_rows) {
    if (visible_rows < 1 || scroll_area_ == nullptr || cards_layout_ == nullptr || layout() == nullptr) {
        return;
    }

    int rows_found = 0;
    int rows_height = 0;
    for (int i = 0; i < cards_layout_->count() && rows_found < visible_rows; ++i) {
        auto* item = cards_layout_->itemAt(i);
        if (item == nullptr) {
            continue;
        }

        QWidget* row = item->widget();
        if (row == nullptr || row->objectName() != QStringLiteral("deviceRow")) {
            continue;
        }

        rows_height += row->sizeHint().height();
        ++rows_found;
    }

    while (rows_found < visible_rows) {
        rows_height += kCollapsedRowHeight;
        ++rows_found;
    }

    const int spacing = cards_layout_->spacing() * std::max(0, visible_rows - 1);
    const QMargins margins = cards_layout_->contentsMargins();
    int list_height = rows_height + spacing + margins.top() + margins.bottom() + kListHeightSlack;

    // Keep window height stable when settings panel is shown/animated:
    // reclaim current panel height from the device list instead of expanding the window.
    if (settings_panel_ != nullptr && settings_panel_->isVisible()) {
        int reclaim_height = std::max(0, settings_panel_->maximumHeight());
        if (auto* root_layout = qobject_cast<QVBoxLayout*>(layout()); root_layout != nullptr) {
            // When settings panel is visible, layout introduces one extra inter-item spacing.
            // Reclaim it even while panel height is still 0 to avoid a one-frame "jump".
            reclaim_height += root_layout->spacing();
        }
        list_height = std::max(kCollapsedRowHeight, list_height - reclaim_height);
    }

    scroll_area_->setFixedHeight(list_height);
    setFixedHeight(layout()->sizeHint().height());
}

void BatteryWindow::UpdateRefreshSettingsTooltip() {
    if (settings_button_ == nullptr) {
        return;
    }
    const int seconds = std::max(kMinRefreshIntervalSeconds, refresh_interval_ms_ / 1000);
    const int threshold = ClampLowBatteryThresholdPercent(low_battery_threshold_percent_);
    const int repeat_minutes = ClampLowBatteryRepeatMinutes(low_battery_repeat_minutes_);
    settings_button_->setToolTip(
        QString::fromUtf8(u8"\u0410\u0432\u0442\u043E\u043E\u0431\u043D\u043E\u0432\u043B\u044F\u0442\u044C "
                          u8"\u043A\u0430\u0436\u0434\u044B\u0435 %1 \u0441\u0435\u043A\n"
                          u8"\u041F\u043E\u0440\u043E\u0433 \u043D\u0438\u0437\u043A\u043E\u0433\u043E "
                          u8"\u0437\u0430\u0440\u044F\u0434\u0430: %2%\n"
                          u8"\u041F\u043E\u0432\u0442\u043E\u0440 \u0443\u0432\u0435\u0434\u043E\u043C\u043B\u0435\u043D\u0438\u0439: %3 \u043C\u0438\u043D")
            .arg(seconds)
            .arg(threshold)
            .arg(repeat_minutes));
}

void BatteryWindow::ApplyRefreshIntervalSeconds(int seconds, bool announce_status) {
    refresh_interval_ms_ = ClampRefreshIntervalMs(seconds * 1000);
    if (refresh_timer_ != nullptr) {
        refresh_timer_->setInterval(refresh_interval_ms_);
    }
    SaveRefreshIntervalMs(refresh_interval_ms_);
    UpdateRefreshSettingsTooltip();

    if (!announce_status || status_label_ == nullptr) {
        return;
    }

    status_label_->setText(QString::fromUtf8(u8"\u0418\u043D\u0442\u0435\u0440\u0432\u0430\u043B "
                                             u8"\u0430\u0432\u0442\u043E\u043E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u0438\u044F: %1 \u0441\u0435\u043A")
                              .arg(refresh_interval_ms_ / 1000));
}

void BatteryWindow::ApplyLowBatteryThresholdPercent(int percent, bool announce_status) {
    const int new_threshold = ClampLowBatteryThresholdPercent(percent);
    const bool threshold_changed = new_threshold != low_battery_threshold_percent_;
    low_battery_threshold_percent_ = new_threshold;
    SaveLowBatteryThresholdPercent(low_battery_threshold_percent_);
    UpdateRefreshSettingsTooltip();

    if (threshold_changed) {
        // Re-arm edge-detection for the new threshold and evaluate current snapshot once.
        last_live_component_levels_.clear();
        last_low_battery_alert_ms_.clear();
        if (!last_devices_snapshot_.empty()) {
            NotifyLowBatteryIfNeeded(last_devices_snapshot_);
        }
    }

    if (!announce_status || status_label_ == nullptr) {
        return;
    }

    status_label_->setText(QString::fromUtf8(u8"\u041F\u043E\u0440\u043E\u0433 "
                                             u8"\u043D\u0438\u0437\u043A\u043E\u0433\u043E "
                                             u8"\u0437\u0430\u0440\u044F\u0434\u0430: %1%")
                              .arg(low_battery_threshold_percent_));
}

void BatteryWindow::ApplyLowBatteryRepeatMinutes(int minutes, bool announce_status) {
    low_battery_repeat_minutes_ = ClampLowBatteryRepeatMinutes(minutes);
    SaveLowBatteryRepeatMinutes(low_battery_repeat_minutes_);
    UpdateRefreshSettingsTooltip();

    if (!announce_status || status_label_ == nullptr) {
        return;
    }

    status_label_->setText(QString::fromUtf8(u8"\u041F\u043E\u0432\u0442\u043E\u0440 "
                                             u8"\u0443\u0432\u0435\u0434\u043E\u043C\u043B\u0435\u043D\u0438\u0439: %1 \u043C\u0438\u043D")
                              .arg(low_battery_repeat_minutes_));
}

void BatteryWindow::ConfigureRefreshInterval() {
    if (settings_panel_ == nullptr || settings_panel_animation_ == nullptr) {
        return;
    }

    settings_panel_expanded_ = !settings_panel_expanded_;
    settings_panel_animation_->stop();

    if (settings_panel_expanded_) {
        settings_panel_->setVisible(true);
        AdjustWindowHeightForRows(kMaxVisibleRows);

        if (refresh_interval_spinbox_ != nullptr) {
            refresh_interval_spinbox_->blockSignals(true);
            refresh_interval_spinbox_->setValue(std::max(kMinRefreshIntervalSeconds, refresh_interval_ms_ / 1000));
            refresh_interval_spinbox_->blockSignals(false);
        }
        if (low_battery_threshold_spinbox_ != nullptr) {
            low_battery_threshold_spinbox_->blockSignals(true);
            low_battery_threshold_spinbox_->setValue(
                ClampLowBatteryThresholdPercent(low_battery_threshold_percent_));
            low_battery_threshold_spinbox_->blockSignals(false);
        }
        if (low_battery_repeat_spinbox_ != nullptr) {
            low_battery_repeat_spinbox_->blockSignals(true);
            low_battery_repeat_spinbox_->setValue(
                ClampLowBatteryRepeatMinutes(low_battery_repeat_minutes_));
            low_battery_repeat_spinbox_->blockSignals(false);
        }

        const int start_height = std::max(0, settings_panel_->maximumHeight());
        const int end_height = std::max(0, settings_panel_->sizeHint().height());
        settings_panel_animation_->setStartValue(start_height);
        settings_panel_animation_->setEndValue(end_height);
        settings_panel_animation_->start();

        QMetaObject::invokeMethod(
            this,
            [this]() {
                if (refresh_interval_spinbox_ != nullptr) {
                    refresh_interval_spinbox_->setFocus();
                    refresh_interval_spinbox_->selectAll();
                }
            },
            Qt::QueuedConnection);
    } else {
        const int start_height = std::max(0, settings_panel_->maximumHeight());
        settings_panel_animation_->setStartValue(start_height);
        settings_panel_animation_->setEndValue(0);
        settings_panel_animation_->start();
    }
}

void BatteryWindow::SetDeviceDragActive(bool active) {
    if (drag_in_progress_ == active) {
        return;
    }

    drag_in_progress_ = active;
    if (drag_in_progress_) {
        return;
    }

    if (refresh_pending_ && !refresh_in_progress_.load(std::memory_order_acquire)) {
        refresh_pending_ = false;
        QMetaObject::invokeMethod(this, [this]() { RefreshBatteryData(); }, Qt::QueuedConnection);
    }
}

void BatteryWindow::RefreshBatteryData() {
    if (drag_in_progress_) {
        refresh_pending_ = true;
        status_label_->setText(QString::fromUtf8(u8"\u041E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u0438\u0435 "
                                                 u8"\u043E\u0442\u043B\u043E\u0436\u0435\u043D\u043E: "
                                                 u8"\u0438\u0434\u0451\u0442 \u043F\u0435\u0440\u0435\u0442\u0430\u0441\u043A\u0438\u0432\u0430\u043D\u0438\u0435"));
        return;
    }

    if (refresh_in_progress_.load(std::memory_order_acquire)) {
        refresh_pending_ = true;
        status_label_->setText(QString::fromUtf8(u8"\u041E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u0438\u0435..."));
        return;
    }

    if (refresh_worker_.joinable()) {
        refresh_worker_.join();
    }
    refresh_in_progress_.store(true, std::memory_order_release);

    refresh_button_->setEnabled(false);
    show_all_button_->setEnabled(false);
    status_label_->setText(QString::fromUtf8(u8"\u041E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u0438\u0435..."));

    refresh_worker_ = std::thread([this]() {
        RefreshTaskResult result;
        try {
            BatteryQueryOptions query_options;
            query_options.include_disconnected = true;
            result.devices = provider_->GetDevicesBattery(query_options);
#ifdef _WIN32
        } catch (const winrt::hresult_error& error) {
            result.error_text = QString::fromUtf8(u8"\u041E\u0448\u0438\u0431\u043A\u0430: %1").arg(FormatWinRtError(error));
            result.is_bluetooth_stack_error = true;
#endif
        } catch (const std::exception& ex) {
            result.error_text = QString::fromUtf8(u8"\u041E\u0448\u0438\u0431\u043A\u0430: %1").arg(ToQString(FormatError(ex)));
        } catch (...) {
            result.error_text = QString::fromUtf8(u8"\u041E\u0448\u0438\u0431\u043A\u0430: \u043D\u0435\u0438\u0437\u0432\u0435\u0441\u0442\u043D\u043E\u0435 \u0438\u0441\u043A\u043B\u044E\u0447\u0435\u043D\u0438\u0435");
        }

        QMetaObject::invokeMethod(
            this,
            [this, result = std::move(result)]() mutable {
                refresh_in_progress_.store(false, std::memory_order_release);

                if (quitting_) {
                    return;
                }

                if (drag_in_progress_) {
                    refresh_button_->setEnabled(true);
                    show_all_button_->setEnabled(!hidden_device_ids_.empty());
                    UpdateToggleActionText();
                    refresh_pending_ = true;
                    status_label_->setText(
                        QString::fromUtf8(u8"\u041E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u0438\u0435 "
                                          u8"\u043E\u0442\u043B\u043E\u0436\u0435\u043D\u043E: "
                                          u8"\u0438\u0434\u0451\u0442 \u043F\u0435\u0440\u0435\u0442\u0430\u0441\u043A\u0438\u0432\u0430\u043D\u0438\u0435"));
                    return;
                }

                if (result.error_text.isEmpty()) {
                    NotifyLowBatteryIfNeeded(result.devices);
                    last_devices_snapshot_ = result.devices;
                    PopulateDeviceCards(result.devices);
                    UpdateTrayTooltip(result.devices);

                    const auto now = QDateTime::currentDateTime();
                    status_label_->setText(
                        QString::fromUtf8(u8"\u041E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u043E \u0432 %1")
                            .arg(now.toString(QStringLiteral("HH:mm:ss"))));
                } else {
                    ClearDeviceCards();
                    summary_label_->setText(QString::fromUtf8(u8"\u041D\u0435\u0442 \u0434\u0430\u043D\u043D\u044B\u0445"));

                    auto* error_label = new QLabel(
                        result.is_bluetooth_stack_error
                            ? QString::fromUtf8(u8"\u041D\u0435 \u0443\u0434\u0430\u043B\u043E\u0441\u044C "
                                                u8"\u043E\u0431\u0440\u0430\u0442\u0438\u0442\u044C\u0441\u044F "
                                                u8"\u043A \u0441\u0442\u0435\u043A\u0443 Bluetooth.")
                            : QString::fromUtf8(u8"\u041D\u0435 \u0443\u0434\u0430\u043B\u043E\u0441\u044C "
                                                u8"\u043F\u043E\u043B\u0443\u0447\u0438\u0442\u044C "
                                                u8"\u0434\u0430\u043D\u043D\u044B\u0435 \u043E \u0437\u0430\u0440\u044F\u0434\u0435."),
                        cards_container_);
                    error_label->setObjectName(QStringLiteral("emptyLabel"));
                    error_label->setAlignment(Qt::AlignCenter);
                    cards_layout_->addWidget(error_label);
                    cards_layout_->addStretch(1);

                    status_label_->setText(result.error_text);
                    AdjustWindowHeightForRows(3);
                }

                refresh_button_->setEnabled(true);
                show_all_button_->setEnabled(!hidden_device_ids_.empty());
                UpdateToggleActionText();

                if (refresh_pending_) {
                    refresh_pending_ = false;
                    RefreshBatteryData();
                }
            },
            Qt::QueuedConnection);
    });
}

void BatteryWindow::PopulateDeviceCards(const std::vector<DeviceBatteryInfo>& devices) {
    ClearDeviceCards();

    const auto grouped = GroupDevices(devices, hidden_device_ids_);
    SyncOrderQueue(grouped, true, &connected_device_order_);
    SyncOrderQueue(grouped, false, &disconnected_device_order_);
    const auto ordered = ApplyCustomOrder(grouped, connected_device_order_, disconnected_device_order_);

    const auto now = QDateTime::currentDateTime();
    const int device_count = static_cast<int>(ordered.size());

    scroll_area_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    for (const auto& device : ordered) {
        if (HasLiveData(device)) {
            last_live_update_[device.device_id] = now;
        }
    }

    const auto counts = ComputeSummaryCounts(ordered);
    summary_label_->setText(BuildSummaryLine(counts, static_cast<int>(hidden_device_ids_.size()), now));

    if (ordered.empty()) {
        auto* empty_label = new QLabel(
            QString::fromUtf8(u8"\u041D\u0435\u0442 \u0432\u0438\u0434\u0438\u043C\u044B\u0445 "
                              u8"\u0443\u0441\u0442\u0440\u043E\u0439\u0441\u0442\u0432. \u041D\u0430\u0436\u043C\u0438\u0442\u0435 "
                              u8"\u00AB\u041F\u043E\u043A\u0430\u0437\u0430\u0442\u044C \u0441\u043A\u0440\u044B\u0442\u044B\u0435\u00BB."),
            cards_container_);
        empty_label->setObjectName(QStringLiteral("emptyLabel"));
        empty_label->setAlignment(Qt::AlignCenter);
        cards_layout_->addWidget(empty_label);
        cards_layout_->addStretch(1);
        AdjustWindowHeightForRows(1);
        return;
    }

    for (const auto& device : ordered) {
        auto* row = new DraggableDeviceRow(device.device_id, IsDeviceConnected(device), cards_container_);
        row->setObjectName(QStringLiteral("deviceRow"));
        row->setFixedHeight(kCollapsedRowHeight);
        row->setMinimumHeight(kCollapsedRowHeight);
        row->setMaximumHeight(kCollapsedRowHeight);
        row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        row->SetDragStateCallback([this](bool active) { SetDeviceDragActive(active); });
        row->SetReorderCallback([this](const std::string& dragged_device_id,
                                       const std::string& target_device_id,
                                       bool connected_queue,
                                       bool insert_before_target) {
            auto& queue = connected_queue ? connected_device_order_ : disconnected_device_order_;
            if (!ReorderQueueItems(&queue, dragged_device_id, target_device_id, insert_before_target)) {
                return;
            }
            SavePersistedDeviceOrder(connected_device_order_, disconnected_device_order_);
            if (last_devices_snapshot_.empty()) {
                return;
            }
            auto devices_snapshot = last_devices_snapshot_;
            QMetaObject::invokeMethod(
                this,
                [this, devices_snapshot = std::move(devices_snapshot)]() {
                    if (quitting_) {
                        return;
                    }
                    if (drag_in_progress_) {
                        refresh_pending_ = true;
                        status_label_->setText(
                            QString::fromUtf8(u8"\u041E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u0438\u0435 "
                                              u8"\u043E\u0442\u043B\u043E\u0436\u0435\u043D\u043E: "
                                              u8"\u0438\u0434\u0451\u0442 \u043F\u0435\u0440\u0435\u0442\u0430\u0441\u043A\u0438\u0432\u0430\u043D\u0438\u0435"));
                        return;
                    }
                    PopulateDeviceCards(devices_snapshot);
                    UpdateTrayTooltip(devices_snapshot);
                    status_label_->setText(
                        QString::fromUtf8(u8"\u041F\u043E\u0440\u044F\u0434\u043E\u043A "
                                          u8"\u0443\u0441\u0442\u0440\u043E\u0439\u0441\u0442\u0432 "
                                          u8"\u043E\u0431\u043D\u043E\u0432\u043B\u0451\u043D"));
                },
                Qt::QueuedConnection);
        });

        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(12, 10, 10, 10);
        row_layout->setSpacing(12);

        auto* icon_label = new QLabel(DeviceTypeCode(device), row);
        icon_label->setObjectName(QStringLiteral("deviceIcon"));
        icon_label->setAlignment(Qt::AlignCenter);
        icon_label->setFixedSize(34, 34);

        auto* center_widget = new QWidget(row);
        auto* center_layout = new QVBoxLayout(center_widget);
        center_layout->setContentsMargins(0, 0, 0, 0);
        center_layout->setSpacing(4);

        const bool is_active = IsDeviceConnected(device);
        const QString active_state = is_active ? QStringLiteral("active") : QStringLiteral("inactive");
        row->setProperty("activeState", active_state);

        auto* name_label = new QLabel(ToQString(device.device_name), center_widget);
        name_label->setObjectName(QStringLiteral("deviceName"));
        name_label->setProperty("activeState", active_state);
        name_label->setWordWrap(false);
        name_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        name_label->setMinimumHeight(20);

        const QString status_text = DeviceStatusText(device, last_live_update_, now);
        const auto primary = ComputePrimaryBattery(device);
        const QString level_state = ProgressLevelState(primary);
        const QString triplet_text = BuildComponentTriplet(device);

        auto* progress = new QProgressBar(center_widget);
        progress->setObjectName(QStringLiteral("deviceProgress"));
        progress->setRange(0, 100);
        progress->setTextVisible(false);
        progress->setProperty("levelState", level_state);
        progress->setProperty("activeState", active_state);
        progress->setValue(primary.level.has_value() ? *primary.level : 0);

        const QString percent_text = primary.level.has_value()
                                         ? QStringLiteral("%1%").arg(*primary.level)
                                         : QString::fromUtf8(u8"\u041D/\u0414");
        auto* percent_chip = new QLabel(percent_text, center_widget);
        percent_chip->setObjectName(QStringLiteral("percentChip"));
        percent_chip->setAlignment(Qt::AlignCenter);
        percent_chip->setMinimumWidth(44);
        percent_chip->setProperty("levelState", level_state);
        percent_chip->setProperty("activeState", active_state);

        QString technical_text = triplet_text;
        if (!is_active) {
            const QString inactive_marker = QString::fromUtf8(u8"\u041D\u0435 \u043F\u043E\u0434\u043A\u043B\u044E\u0447\u0435\u043D\u043E");
            technical_text = technical_text.isEmpty() ? inactive_marker
                                                      : technical_text + QStringLiteral("  \u00B7  ") + inactive_marker;
        }

        auto* technical_label =
            new QLabel(technical_text.isEmpty() ? QStringLiteral(" ") : technical_text, center_widget);
        technical_label->setObjectName(QStringLiteral("technicalMeta"));
        technical_label->setProperty("activeState", active_state);
        technical_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        technical_label->setFixedHeight(14);

        auto* battery_layout = new QHBoxLayout();
        battery_layout->setContentsMargins(0, 0, 0, 0);
        battery_layout->setSpacing(6);
        battery_layout->addWidget(progress, 1);
        battery_layout->addWidget(percent_chip, 0, Qt::AlignVCenter);

        QString details_tooltip = status_text;
        if (!triplet_text.isEmpty()) {
            details_tooltip += QStringLiteral("\n") + triplet_text;
        }
        progress->setToolTip(details_tooltip);
        percent_chip->setToolTip(details_tooltip);
        technical_label->setToolTip(details_tooltip);

        center_layout->addWidget(name_label);
        center_layout->addLayout(battery_layout);
        center_layout->addWidget(technical_label);
        center_layout->addStretch(1);

        auto* actions_button = new QToolButton(row);
        actions_button->setObjectName(QStringLiteral("inlineMenuButton"));
        actions_button->setText(QString::fromUtf8(u8"\u22EF"));
        actions_button->setPopupMode(QToolButton::InstantPopup);
        actions_button->setToolTip(QString::fromUtf8(u8"\u0414\u0435\u0439\u0441\u0442\u0432\u0438\u044F"));

        auto* actions_menu = new QMenu(actions_button);
        auto* refresh_row_action = actions_menu->addAction(QString::fromUtf8(u8"\u041E\u0431\u043D\u043E\u0432\u0438\u0442\u044C"));
        auto* hide_row_action =
            actions_menu->addAction(QString::fromUtf8(u8"\u0421\u043A\u0440\u044B\u0442\u044C "
                                                      u8"\u0443\u0441\u0442\u0440\u043E\u0439\u0441\u0442\u0432\u043E"));
        actions_button->setMenu(actions_menu);

        connect(refresh_row_action, &QAction::triggered, this, [this]() { RefreshBatteryData(); });
        connect(hide_row_action, &QAction::triggered, this, [this, device_id = device.device_id]() {
            hidden_device_ids_.insert(device_id);
            RefreshBatteryData();
        });

        row_layout->addWidget(icon_label, 0, Qt::AlignVCenter);
        row_layout->addWidget(center_widget, 1);
        row_layout->addWidget(actions_button, 0, Qt::AlignVCenter);

        cards_layout_->addWidget(row);
    }

    cards_layout_->addStretch(1);
    AdjustWindowHeightForRows(std::min(kMaxVisibleRows, std::max(1, device_count)));
}

void BatteryWindow::ClearDeviceCards() {
    if (cards_layout_ == nullptr) {
        return;
    }

    QLayoutItem* item = nullptr;
    while ((item = cards_layout_->takeAt(0)) != nullptr) {
        if (item->widget() != nullptr) {
            item->widget()->deleteLater();
        }
        delete item;
    }
}

std::string BatteryWindow::FormatError(const std::exception& ex) {
    return ex.what() == nullptr ? std::string("Unknown error") : std::string(ex.what());
}

}  // namespace battery_monitor
