#include "ui/BatteryWindow.h"
#include "ui/BatteryHistoryDialog.h"
#include "ui/BatteryRuntimeEstimator.h"
#include "ui/NoiseControlUi.h"
#include "ui/BatteryStatsDialog.h"
#include "ui/BatteryWindowSettings.h"
#include <QRandomGenerator>
#ifdef BATTERY_MONITOR_WITH_UPDATER
#include "ui/UpdateDialog.h"
#include "update/UpdateService.h"
#include "BatteryMonitorVersion.h"
#endif
#include "ui/DeviceDiagnosticsDialog.h"
#include "ui/DraggableDeviceRow.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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
#include <QIcon>
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
#include <QPixmap>
#include <QPushButton>
#include <QResource>
#include <QStandardPaths>
#include <QScrollArea>
#include <QScreen>
#include <QSettings>
#include <QSizePolicy>
#include <QSvgRenderer>
#include <QSpinBox>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include "core/NoiseControlVocabulary.h"

#ifdef _WIN32
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>
#endif

#ifdef _WIN32
#include "platform/windows/shared/WindowsBluetoothAddressUtils.h"
#include "platform/windows/shared/WindowsBatteryProviderSupport.h"
#endif

namespace battery_monitor {

namespace {

bool UiDebugEnabled() {
    const QByteArray value = qgetenv("BATTERY_MONITOR_DEBUG");
    return !value.isEmpty() && value != "0" && value.toLower() != "false";
}

std::filesystem::path UiDebugLogPath() {
    QString base_path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!base_path.trimmed().isEmpty()) {
        return std::filesystem::path(base_path.toStdWString()) / "diagnostics" / "ui-debug.log";
    }
    return std::filesystem::current_path() / "ui-debug.log";
}

void UiDebugLog(const std::string& message) {
#ifdef _WIN32
    if (UiDebugEnabled()) {
        WindowsBatteryProviderDebugLog("UI " + message);
    }
#else
    if (!UiDebugEnabled()) {
        return;
    }

    static std::mutex log_mutex;
    std::lock_guard lock(log_mutex);

    const auto path = UiDebugLogPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream stream(path, std::ios::out | std::ios::app);
    if (!stream) {
        return;
    }
    stream << QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toStdString()
           << " [ui] " << message << '\n';
#endif
}

std::string OptionalLevelText(const std::optional<std::uint8_t>& value) {
    if (!value.has_value()) {
        return "na";
    }
    return std::to_string(static_cast<int>(*value));
}

std::string OptionalText(const std::optional<std::string>& value) {
    return value.has_value() ? *value : "na";
}

void UiDebugLogDevices(const std::string& stage, const std::vector<DeviceBatteryInfo>& devices) {
    if (!UiDebugEnabled()) {
        return;
    }

    UiDebugLog(stage + " devices=" + std::to_string(devices.size()));
    for (std::size_t index = 0; index < devices.size(); ++index) {
        const auto& device = devices[index];
        std::ostringstream line;
        line << stage << "[" << index << "]"
             << " id='" << device.device_id << "'"
             << " name='" << device.device_name << "'"
             << " component='" << device.battery_component << "'"
             << " level=" << OptionalLevelText(device.battery_level_percent)
             << " cached=" << (device.is_cached ? "true" : "false")
             << " connected=" << (device.is_connected ? "true" : "false")
             << " mode='" << OptionalText(device.device_mode) << "'"
             << " submode='" << OptionalText(device.device_submode) << "'";
        UiDebugLog(line.str());
    }
}

std::uint64_t NextUiRefreshId() {
    static std::atomic<std::uint64_t> next_id{1};
    return next_id.fetch_add(1, std::memory_order_relaxed);
}

const bool kDeviceIconsResourceInitialized = []() {
    Q_INIT_RESOURCE(device_icons);
    return true;
}();

struct ComponentEntry {
    std::string component;
    std::optional<std::uint8_t> battery_level_percent;
    bool is_cached = false;
    bool is_connected = true;
};

struct DeviceEntry {
    std::string device_id;
    std::string device_name;
    std::optional<std::string> device_mode;
    std::optional<std::string> device_submode;
    std::optional<std::uint16_t> bluetooth_le_appearance;
    std::optional<std::uint32_t> bluetooth_cod_major;
    std::optional<std::uint32_t> bluetooth_cod_minor;
    std::vector<std::string> device_categories;
    std::vector<ComponentEntry> components;
    bool is_connected = false;
};

void UiDebugLogGroupedDevices(const std::string& stage, const std::vector<DeviceEntry>& devices) {
    if (!UiDebugEnabled()) {
        return;
    }

    UiDebugLog(stage + " grouped=" + std::to_string(devices.size()));
    for (std::size_t index = 0; index < devices.size(); ++index) {
        const auto& device = devices[index];
        std::ostringstream line;
        line << stage << "[" << index << "]"
             << " id='" << device.device_id << "'"
             << " name='" << device.device_name << "'"
             << " connected=" << (device.is_connected ? "true" : "false")
             << " mode='" << OptionalText(device.device_mode) << "'"
             << " components=";
        for (const auto& component : device.components) {
            line << component.component << ":" << OptionalLevelText(component.battery_level_percent)
                 << (component.is_cached ? ":cached" : ":live")
                 << (component.is_connected ? ":connected" : ":disconnected") << ";";
        }
        UiDebugLog(line.str());
    }
}

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

struct SmoothButtonColors {
    QColor fill;
    QColor border;
    QColor text;
    qreal radius = 10.0;
};

constexpr int kMaxVisibleRows = 3;
constexpr int kCollapsedRowHeight = 88;
constexpr int kNoiseControlRowHeight = 116;
constexpr int kListPadding = 12;
constexpr int kListSpacing = 10;
constexpr int kListHeightSlack = 28;
constexpr int kDefaultWindowWidth = 390;
constexpr int kNoiseControlWindowWidth = 470;
QString ToQString(const std::string& value) {
    return QString::fromUtf8(value.c_str());
}

SmoothButtonColors ResolveSmoothButtonColors(const QWidget* button, bool pressed, bool hovered, bool enabled) {
    const QString kind = button != nullptr ? button->property("smoothKind").toString() : QString();
    if (kind == QStringLiteral("inline")) {
        return SmoothButtonColors{
            hovered || pressed ? QColor(255, 255, 255, pressed ? 34 : 20) : QColor(0, 0, 0, 0),
            QColor(0, 0, 0, 0),
            hovered || pressed ? QColor(QStringLiteral("#D6DCE7")) : QColor(QStringLiteral("#A7B0C0")),
            8.0,
        };
    }

    if (kind == QStringLiteral("settings")) {
        return SmoothButtonColors{
            QColor(pressed ? QStringLiteral("#383C43") : (hovered ? QStringLiteral("#4D525B") : QStringLiteral("#40444B"))),
            QColor(255, 255, 255, 36),
            QColor(QStringLiteral("#E7EDF8")),
            10.0,
        };
    }

    if (kind == QStringLiteral("noise")) {
        const bool active = button != nullptr && button->property("activeMode").toBool();
        return SmoothButtonColors{
            QColor(active ? QStringLiteral("#4E5968")
                          : (pressed ? QStringLiteral("#383C43")
                                     : (hovered ? QStringLiteral("#4D525B") : QStringLiteral("#40444B")))),
            active ? QColor(116, 190, 255, 130) : QColor(255, 255, 255, 34),
            QColor(QStringLiteral("#F2F5FB")),
            9.0,
        };
    }

    return SmoothButtonColors{
        QColor(!enabled ? QStringLiteral("#373A40")
                        : (pressed ? QStringLiteral("#3C4149")
                                   : (hovered ? QStringLiteral("#50555E") : QStringLiteral("#44484F")))),
        QColor(255, 255, 255, enabled ? 36 : 20),
        QColor(enabled ? QStringLiteral("#F8FAFC") : QStringLiteral("#9CA3AF")),
        10.0,
    };
}

void PaintSmoothButton(QPainter* painter,
                       const QWidget* widget,
                       const QString& text,
                       const QFont& font,
                       bool pressed,
                       bool hovered,
                       bool enabled) {
    if (painter == nullptr || widget == nullptr) {
        return;
    }

    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    const SmoothButtonColors colors = ResolveSmoothButtonColors(widget, pressed, hovered, enabled);
    QRectF button_rect = widget->rect();
    button_rect.adjust(0.5, 0.5, -0.5, -0.5);

    QPainterPath path;
    path.addRoundedRect(button_rect, colors.radius, colors.radius);
    painter->fillPath(path, colors.fill);
    if (colors.border.alpha() > 0) {
        painter->setPen(QPen(colors.border, 1.0));
        painter->drawPath(path);
    }

    painter->setFont(font);
    painter->setPen(colors.text);
    painter->drawText(widget->rect().adjusted(8, 0, -8, 0), Qt::AlignCenter, text);
}

class SmoothPushButton final : public QPushButton {
   public:
    using QPushButton::QPushButton;

   protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        PaintSmoothButton(&painter, this, text(), font(), isDown(), underMouse(), isEnabled());
    }

    void enterEvent(QEnterEvent* event) override {
        QPushButton::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent* event) override {
        QPushButton::leaveEvent(event);
        update();
    }

    void mousePressEvent(QMouseEvent* event) override {
        QPushButton::mousePressEvent(event);
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        QPushButton::mouseReleaseEvent(event);
        update();
    }
};

class SmoothToolButton final : public QToolButton {
   public:
    using QToolButton::QToolButton;

   protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        PaintSmoothButton(&painter, this, text(), font(), isDown(), underMouse(), isEnabled());
    }

    void enterEvent(QEnterEvent* event) override {
        QToolButton::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent* event) override {
        QToolButton::leaveEvent(event);
        update();
    }

    void mousePressEvent(QMouseEvent* event) override {
        QToolButton::mousePressEvent(event);
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        QToolButton::mouseReleaseEvent(event);
        update();
    }
};

class SmoothFrame final : public QFrame {
   public:
    using QFrame::QFrame;

   protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QRectF frame_rect = rect();
        frame_rect.adjust(0.5, 0.5, -0.5, -0.5);
        QPainterPath path;
        path.addRoundedRect(frame_rect, 10.0, 10.0);
        painter.fillPath(path, QColor(QStringLiteral("#3A3E45")));
        painter.setPen(QPen(QColor(255, 255, 255, 26), 1.0));
        painter.drawPath(path);
    }
};

class SmoothPercentChip final : public QLabel {
   public:
    using QLabel::QLabel;

   protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);

        const bool inactive = property("activeState").toString() == QStringLiteral("inactive");
        const bool low = property("levelState").toString() == QStringLiteral("low");
        const QColor fill = inactive ? QColor(QStringLiteral("#262B33"))
                                     : (low ? QColor(QStringLiteral("#41242A")) : QColor(QStringLiteral("#2A2F37")));
        const QColor border = inactive ? QColor(255, 255, 255, 26)
                                       : (low ? QColor(QStringLiteral("#8E404A")) : QColor(255, 255, 255, 36));
        const QColor text_color = inactive ? QColor(QStringLiteral("#B7BFCD"))
                                           : (low ? QColor(QStringLiteral("#FFD0D5")) : QColor(QStringLiteral("#E7ECF6")));

        QRectF chip_rect = rect();
        chip_rect.adjust(0.5, 0.5, -0.5, -0.5);
        QPainterPath path;
        path.addRoundedRect(chip_rect, 9.0, 9.0);
        painter.fillPath(path, fill);
        painter.setPen(QPen(border, 1.0));
        painter.drawPath(path);
        painter.setFont(font());
        painter.setPen(text_color);
        painter.drawText(rect().adjusted(7, 0, -7, 0), Qt::AlignCenter, text());
    }
};

class SmoothProgressBar final : public QProgressBar {
   public:
    using QProgressBar::QProgressBar;

   protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const bool inactive = property("activeState").toString() == QStringLiteral("inactive");
        const QString state = property("levelState").toString();
        QColor chunk(QStringLiteral("#30C26E"));
        if (state == QStringLiteral("warn")) {
            chunk = QColor(QStringLiteral("#D7B446"));
        } else if (state == QStringLiteral("low")) {
            chunk = QColor(QStringLiteral("#E06767"));
        } else if (state == QStringLiteral("na") || inactive) {
            chunk = QColor(QStringLiteral("#5F6876"));
        }

        QRectF track_rect = rect();
        track_rect.adjust(0.5, 0.5, -0.5, -0.5);
        QPainterPath track_path;
        track_path.addRoundedRect(track_rect, 6.0, 6.0);
        painter.fillPath(track_path, QColor(QStringLiteral("#171A20")));
        painter.setPen(QPen(inactive ? QColor(255, 255, 255, 13) : QColor(255, 255, 255, 20), 1.0));
        painter.drawPath(track_path);

        if (maximum() <= minimum() || value() <= minimum()) {
            return;
        }
        const double ratio = std::clamp(
            static_cast<double>(value() - minimum()) / static_cast<double>(maximum() - minimum()), 0.0, 1.0);
        QRectF chunk_rect = track_rect.adjusted(1.0, 1.0, -1.0, -1.0);
        chunk_rect.setWidth(std::max<qreal>(chunk_rect.height(), chunk_rect.width() * ratio));
        QPainterPath chunk_path;
        chunk_path.addRoundedRect(chunk_rect, 5.0, 5.0);
        painter.fillPath(chunk_path, chunk);
    }
};

class SmoothSpinBox final : public QSpinBox {
   public:
    using QSpinBox::QSpinBox;

   protected:
    void paintEvent(QPaintEvent* event) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QRectF box_rect = rect();
        box_rect.adjust(0.5, 0.5, -0.5, -0.5);
        QPainterPath path;
        path.addRoundedRect(box_rect, 8.0, 8.0);
        painter.fillPath(path, QColor(QStringLiteral("#2E3238")));
        painter.setPen(QPen(QColor(255, 255, 255, 36), 1.0));
        painter.drawPath(path);
        QSpinBox::paintEvent(event);
    }
};

std::string ToLowerAsciiCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

#ifdef _WIN32
bool DeviceIdContainsBluetoothAddress(std::string_view device_id, std::uint64_t address) {
    std::ostringstream compact_builder;
    compact_builder << std::nouppercase << std::hex << std::setw(12) << std::setfill('0') << address;
    const std::string compact = compact_builder.str();

    std::ostringstream colon_builder;
    for (std::size_t i = 0; i < compact.size(); i += 2) {
        if (i != 0) {
            colon_builder << ':';
        }
        colon_builder << compact.substr(i, 2);
    }

    const std::string lowered_id = ToLowerAsciiCopy(std::string(device_id));
    return lowered_id.find(compact) != std::string::npos ||
           lowered_id.find(colon_builder.str()) != std::string::npos;
}
#endif

bool DeviceIdsReferToSameBluetoothDevice(std::string_view lhs, std::string_view rhs) {
    if (lhs.empty() || rhs.empty()) {
        return false;
    }
    if (lhs == rhs) {
        return true;
    }
#ifdef _WIN32
    const auto lhs_address = ParseBluetoothAddressFromDeviceId(std::string(lhs));
    const auto rhs_address = ParseBluetoothAddressFromDeviceId(std::string(rhs));
    if (lhs_address.has_value() && rhs_address.has_value()) {
        return *lhs_address == *rhs_address;
    }
    if (lhs_address.has_value()) {
        return DeviceIdContainsBluetoothAddress(rhs, *lhs_address);
    }
    if (rhs_address.has_value()) {
        return DeviceIdContainsBluetoothAddress(lhs, *rhs_address);
    }
#endif
    const std::string lowered_lhs = ToLowerAsciiCopy(std::string(lhs));
    const std::string lowered_rhs = ToLowerAsciiCopy(std::string(rhs));
    return lowered_lhs.find(lowered_rhs) != std::string::npos || lowered_rhs.find(lowered_lhs) != std::string::npos;
}

std::string DeviceGroupingKey(const std::string& device_id) {
#ifdef _WIN32
    const auto address = ParseBluetoothAddressFromDeviceId(device_id);
    if (address.has_value()) {
        return "btaddr:" + std::to_string(*address);
    }
#endif
    return device_id.empty() ? "UnknownDevice" : device_id;
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

QString BuildDeviceModeText(const DeviceEntry& device) {
    if (!device.device_mode.has_value()) {
        return {};
    }

    const QString mode_label = NoiseModeLabel(ToQString(*device.device_mode));
    if (mode_label.isEmpty()) {
        return {};
    }

    const std::string mode = NormalizeNoiseControlToken(*device.device_mode);
    const QString normalized_submode =
        device.device_submode.has_value() ? NormalizeNoiseToken(ToQString(*device.device_submode)) : QString();
    const QString submode_label =
        device.device_submode.has_value() ? NoiseSubmodeLabel(ToQString(*device.device_submode)) : QString();
    const std::string submode = normalized_submode.toStdString();
    auto append_submode = [&](QString base_text) {
        if (submode_label.isEmpty() || normalized_submode == QStringLiteral("standard")) {
            return base_text;
        }

        if (submode == "balanced") {
            return base_text + QString::fromUtf8(u8" · Баланс");
        }
        if (submode == "weak") {
            return base_text + QString::fromUtf8(u8" · Слабое");
        }
        if (submode == "deep") {
            return base_text + QString::fromUtf8(u8" · Глубокое");
        }
        if (submode == "adaptive") {
            return base_text + QString::fromUtf8(u8" · Адаптивное");
        }
        if (submode == "voice") {
            return base_text + QString::fromUtf8(u8" · Усиление голоса");
        }
        if (submode == "standard") {
            return base_text;
        }

        return base_text + QString::fromUtf8(u8" · %1").arg(ToQString(submode));
    };

    if (mode == "off") {
        return QString::fromUtf8(u8"\u0420\u0435\u0436\u0438\u043C: \u0432\u044B\u043A\u043B");
    }
    if (mode == "transparency") {
        return append_submode(
            QString::fromUtf8(u8"\u0420\u0435\u0436\u0438\u043C: \u043F\u0440\u043E\u0437\u0440\u0430\u0447\u043D\u043E\u0441\u0442\u044C"));
    }
    if (mode == "anc") {
        return append_submode(
            QString::fromUtf8(u8"\u0420\u0435\u0436\u0438\u043C: \u0448\u0443\u043C\u043E\u043F\u043E\u0434\u0430\u0432\u043B\u0435\u043D\u0438\u0435"));
    }

    return append_submode(QString::fromUtf8(u8"\u0420\u0435\u0436\u0438\u043C: %1").arg(mode_label));
}

QString FormatRuntimeCountdownNoSeconds(qint64 duration_ms) {
    if (duration_ms <= 0) {
        return QString::fromUtf8(u8"Осталось: меньше минуты");
    }

    const qint64 total_minutes = std::max<qint64>(1, duration_ms / (60LL * 1000LL));
    const qint64 hours = total_minutes / 60LL;
    const qint64 minutes = total_minutes % 60LL;
    if (hours <= 0) {
        return QString::fromUtf8(u8"Осталось: %1 м").arg(total_minutes);
    }
    if (minutes == 0) {
        return QString::fromUtf8(u8"Осталось: %1 ч").arg(hours);
    }
    return QString::fromUtf8(u8"Осталось: %1 ч %2 м").arg(hours).arg(minutes);
}

QColor TrayLevelColor(int level) {
    if (level < 20) {
        return QColor(QStringLiteral("#FF7A7A"));
    }
    if (level < 40) {
        return QColor(QStringLiteral("#FFD166"));
    }
    return QColor(QStringLiteral("#F8FAFC"));
}

QIcon BuildTrayLevelIcon(int level) {
    const QString text = QString::number(level);
    const QColor fill_color = TrayLevelColor(level);
    const QColor shadow_color(0, 0, 0, 150);

    auto build_pixmap = [&](int size) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);

        QFont font(QStringLiteral("Segoe UI"));
        font.setBold(true);
        if (text.size() >= 3) {
            font.setPixelSize(std::max(8, static_cast<int>(std::round(size * 0.58))));
        } else {
            font.setPixelSize(std::max(9, static_cast<int>(std::round(size * 0.86))));
        }
        painter.setFont(font);

        QPainterPath path;
        path.addText(0, 0, font, text);
        QRectF bounds = path.boundingRect();
        const qreal x = (static_cast<qreal>(size) - bounds.width()) / 2.0 - bounds.left();
        const qreal y = (static_cast<qreal>(size) - bounds.height()) / 2.0 - bounds.top();

        QTransform transform;
        transform.translate(x, y);
        path = transform.map(path);

        QPainterPath shadow_path = path;
        shadow_path.translate(std::max<qreal>(1.0, size / 18.0), std::max<qreal>(1.0, size / 18.0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(shadow_color);
        painter.drawPath(shadow_path);

        painter.setBrush(fill_color);
        painter.drawPath(path);

        return pixmap;
    };

    QIcon icon;
    for (const int size : {16, 20, 24, 32, 40, 48, 64}) {
        icon.addPixmap(build_pixmap(size));
    }
    return icon;
}

qint64 ResolveStickyRuntimeDeadline(qint64 current_deadline_ms, qint64 proposed_deadline_ms) {
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    if (proposed_deadline_ms <= current_deadline_ms) {
        return proposed_deadline_ms;
    }
    if (current_deadline_ms <= now_ms) {
        return proposed_deadline_ms;
    }

    constexpr qint64 kHardCorrectionThresholdMs = 45LL * 60LL * 1000LL;
    const qint64 current_remaining_ms = current_deadline_ms - now_ms;
    const qint64 delta_ms = proposed_deadline_ms - current_deadline_ms;
    if (delta_ms >= kHardCorrectionThresholdMs || delta_ms >= (current_remaining_ms / 2LL)) {
        return proposed_deadline_ms;
    }

    return current_deadline_ms;
}

std::optional<std::uint8_t> ComponentLevel(const DeviceEntry& device, const char* name) {
    if (const auto* component = FindComponent(device, name); component != nullptr) {
        return component->battery_level_percent;
    }
    return std::nullopt;
}

std::string BuildRuntimeStateKey(const DeviceEntry& device,
                                 const QString& selected_component_key,
                                 bool use_pair_forecast) {
    std::ostringstream stream;
    stream << (use_pair_forecast ? "pair" : selected_component_key.toStdString()) << '|';

    if (use_pair_forecast) {
        const auto left = ComponentLevel(device, "left");
        const auto right = ComponentLevel(device, "right");
        stream << (left.has_value() ? std::to_string(*left) : std::string("na")) << '|'
               << (right.has_value() ? std::to_string(*right) : std::string("na"));
    } else {
        const auto component = FindComponent(device, selected_component_key.toUtf8().constData());
        const auto level = component != nullptr ? component->battery_level_percent : std::optional<std::uint8_t>{};
        stream << (level.has_value() ? std::to_string(*level) : std::string("na"));
    }

    stream << '|';
    if (device.device_mode.has_value()) {
        stream << NormalizeNoiseControlToken(*device.device_mode);
    }
    stream << '|';
    if (device.device_submode.has_value()) {
        stream << NormalizeNoiseControlToken(*device.device_submode);
    }
    return stream.str();
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

enum class DeviceVisualType {
    Headphones,
    Gamepad,
    Phone,
    Keyboard,
    Mouse,
    Speaker,
    Watch,
    Computer,
    Laptop,
    Generic,
};

struct DeviceVisualDecision {
    DeviceVisualType type = DeviceVisualType::Generic;
    QString reason;
};

bool HasAnyComponent(const DeviceEntry& device, std::initializer_list<const char*> component_names) {
    for (const auto& component : device.components) {
        for (const char* name : component_names) {
            if (component.component == name) {
                return true;
            }
        }
    }
    return false;
}

std::string JoinStrings(const std::vector<std::string>& values, const char* separator) {
    std::ostringstream stream;
    bool first = true;
    for (const auto& value : values) {
        if (value.empty()) {
            continue;
        }
        if (!first) {
            stream << separator;
        }
        stream << value;
        first = false;
    }
    return stream.str();
}

bool CategoryContains(const std::vector<std::string>& categories, std::initializer_list<const char*> needles) {
    for (const auto& category : categories) {
        const std::string probe = ToLowerAscii(category);
        for (const char* needle : needles) {
            if (probe.find(needle) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

QString DeviceVisualTypeLabel(DeviceVisualType type) {
    switch (type) {
    case DeviceVisualType::Headphones:
        return QString::fromUtf8(u8"Наушники");
    case DeviceVisualType::Gamepad:
        return QString::fromUtf8(u8"Геймпад");
    case DeviceVisualType::Phone:
        return QString::fromUtf8(u8"Телефон");
    case DeviceVisualType::Keyboard:
        return QString::fromUtf8(u8"Клавиатура");
    case DeviceVisualType::Mouse:
        return QString::fromUtf8(u8"Мышка");
    case DeviceVisualType::Speaker:
        return QString::fromUtf8(u8"Колонка");
    case DeviceVisualType::Watch:
        return QString::fromUtf8(u8"Часы");
    case DeviceVisualType::Computer:
        return QString::fromUtf8(u8"ПК");
    case DeviceVisualType::Laptop:
        return QString::fromUtf8(u8"Ноутбук");
    case DeviceVisualType::Generic:
    default:
        return QString::fromUtf8(u8"Bluetooth");
    }
}

std::optional<DeviceVisualDecision> DetectDeviceVisualTypeFromBluetoothHints(const DeviceEntry& device) {
    if (!device.device_categories.empty()) {
        const QString category_text = ToQString(JoinStrings(device.device_categories, ", "));
        if (CategoryContains(device.device_categories, {"input.keyboard", "keyboard"})) {
            return DeviceVisualDecision{
                DeviceVisualType::Keyboard,
                QString::fromUtf8(u8"Категория Windows: %1").arg(category_text)};
        }
        if (CategoryContains(device.device_categories, {"input.mouse", "mouse", "pointing"})) {
            return DeviceVisualDecision{
                DeviceVisualType::Mouse,
                QString::fromUtf8(u8"Категория Windows: %1").arg(category_text)};
        }
        if (CategoryContains(device.device_categories, {"game.controller", "gamepad", "controller", "joystick"})) {
            return DeviceVisualDecision{
                DeviceVisualType::Gamepad,
                QString::fromUtf8(u8"Категория Windows: %1").arg(category_text)};
        }
        if (CategoryContains(device.device_categories, {"audio.head", "headphone", "headset", "earbud"})) {
            return DeviceVisualDecision{
                DeviceVisualType::Headphones,
                QString::fromUtf8(u8"Категория Windows: %1").arg(category_text)};
        }
        if (CategoryContains(device.device_categories, {"audio.speaker", "speaker"})) {
            return DeviceVisualDecision{
                DeviceVisualType::Speaker,
                QString::fromUtf8(u8"Категория Windows: %1").arg(category_text)};
        }
        if (CategoryContains(device.device_categories, {"phone", "mobile"})) {
            return DeviceVisualDecision{
                DeviceVisualType::Phone,
                QString::fromUtf8(u8"Категория Windows: %1").arg(category_text)};
        }
        if (CategoryContains(device.device_categories, {"watch", "wearable", "band"})) {
            return DeviceVisualDecision{
                DeviceVisualType::Watch,
                QString::fromUtf8(u8"Категория Windows: %1").arg(category_text)};
        }
        if (CategoryContains(device.device_categories, {"computer", "pc", "desktop", "workstation"})) {
            return DeviceVisualDecision{
                DeviceVisualType::Computer,
                QString::fromUtf8(u8"Категория Windows: %1").arg(category_text)};
        }
        if (CategoryContains(device.device_categories, {"laptop", "notebook"})) {
            return DeviceVisualDecision{
                DeviceVisualType::Laptop,
                QString::fromUtf8(u8"Категория Windows: %1").arg(category_text)};
        }
    }

    if (device.bluetooth_le_appearance.has_value()) {
        switch (*device.bluetooth_le_appearance) {
        case 64:
            return DeviceVisualDecision{
                DeviceVisualType::Phone,
                QString::fromUtf8(u8"Bluetooth LE Appearance = 64 (Phone)")};
        case 128:
        case 129:
        case 130:
        case 132:
        case 133:
        case 134:
        case 135:
            return DeviceVisualDecision{
                DeviceVisualType::Computer,
                QString::fromUtf8(u8"Bluetooth LE Appearance = %1 (Computer)").arg(*device.bluetooth_le_appearance)};
        case 131:
            return DeviceVisualDecision{
                DeviceVisualType::Laptop,
                QString::fromUtf8(u8"Bluetooth LE Appearance = 131 (Laptop)")};
        case 192:
        case 193:
            return DeviceVisualDecision{
                DeviceVisualType::Watch,
                QString::fromUtf8(u8"Bluetooth LE Appearance = %1 (Watch)").arg(*device.bluetooth_le_appearance)};
        case 961:
            return DeviceVisualDecision{
                DeviceVisualType::Keyboard,
                QString::fromUtf8(u8"Bluetooth LE Appearance = 961 (Keyboard)")};
        case 962:
            return DeviceVisualDecision{
                DeviceVisualType::Mouse,
                QString::fromUtf8(u8"Bluetooth LE Appearance = 962 (Mouse)")};
        case 963:
        case 964:
            return DeviceVisualDecision{
                DeviceVisualType::Gamepad,
                QString::fromUtf8(u8"Bluetooth LE Appearance = %1 (Gamepad)").arg(*device.bluetooth_le_appearance)};
        default:
            break;
        }
    }

    if (!device.bluetooth_cod_major.has_value()) {
        return std::nullopt;
    }

    const std::uint32_t major = *device.bluetooth_cod_major;
    const std::uint32_t minor = device.bluetooth_cod_minor.value_or(0);
    switch (major) {
    case 1:
        if (minor == 3U) {
            return DeviceVisualDecision{
                DeviceVisualType::Laptop,
                QString::fromUtf8(u8"Bluetooth Class of Device major = 1 (Computer), minor = 3 (Laptop)")};
        }
        return DeviceVisualDecision{
            DeviceVisualType::Computer,
            QString::fromUtf8(u8"Bluetooth Class of Device major = 1 (Computer), minor = %1").arg(minor)};
    case 2:
        return DeviceVisualDecision{
            DeviceVisualType::Phone,
            QString::fromUtf8(u8"Bluetooth Class of Device major = 2 (Phone), minor = %1").arg(minor)};
    case 4:
        if (HasAnyComponent(device, {"left", "right", "case"})) {
            return DeviceVisualDecision{
                DeviceVisualType::Headphones,
                QString::fromUtf8(u8"Bluetooth Class of Device major = 4 (Audio/Video), а батарея разбита на left/right/case")};
        }
        return DeviceVisualDecision{
            DeviceVisualType::Speaker,
            QString::fromUtf8(u8"Bluetooth Class of Device major = 4 (Audio/Video), minor = %1").arg(minor)};
    case 5:
        if ((minor & 0x80U) != 0U) {
            return DeviceVisualDecision{
                DeviceVisualType::Mouse,
                QString::fromUtf8(u8"Bluetooth Class of Device major = 5 (Peripheral), minor = 0x%1 с флагом pointing device")
                    .arg(minor, 0, 16)};
        }
        if ((minor & 0x40U) != 0U) {
            return DeviceVisualDecision{
                DeviceVisualType::Keyboard,
                QString::fromUtf8(u8"Bluetooth Class of Device major = 5 (Peripheral), minor = 0x%1 с флагом keyboard")
                    .arg(minor, 0, 16)};
        }
        if ((minor & 0x0FU) >= 0x01U && (minor & 0x0FU) <= 0x05U) {
            return DeviceVisualDecision{
                DeviceVisualType::Gamepad,
                QString::fromUtf8(u8"Bluetooth Class of Device major = 5 (Peripheral), minor = 0x%1 похож на game controller")
                    .arg(minor, 0, 16)};
        }
        break;
    case 7:
        return DeviceVisualDecision{
            DeviceVisualType::Watch,
            QString::fromUtf8(u8"Bluetooth Class of Device major = 7 (Wearable), minor = %1").arg(minor)};
    case 8:
        return DeviceVisualDecision{
            DeviceVisualType::Gamepad,
            QString::fromUtf8(u8"Bluetooth Class of Device major = 8 (Toy/Game), minor = %1").arg(minor)};
    default:
        break;
    }

    return std::nullopt;
}

DeviceVisualDecision DetectDeviceVisualDecision(const DeviceEntry& device) {
    if (const auto hinted_type = DetectDeviceVisualTypeFromBluetoothHints(device); hinted_type.has_value()) {
        return *hinted_type;
    }

    if (HasAnyComponent(device, {"left", "right", "case"})) {
        return {DeviceVisualType::Headphones,
                QString::fromUtf8(u8"По компонентам батареи устройство похоже на TWS/наушники")};
    }

    if (!device.device_categories.empty()) {
        return {
            DeviceVisualType::Generic,
            QString::fromUtf8(u8"Категории Windows: %1, но тип по ним не распознан")
                .arg(ToQString(JoinStrings(device.device_categories, ", ")))};
    }

    return {DeviceVisualType::Generic,
            QString::fromUtf8(u8"Bluetooth-подсказок не найдено, использован общий значок")};
}

DeviceVisualType DetectDeviceVisualType(const DeviceEntry& device) {
    return DetectDeviceVisualDecision(device).type;
}

QString BuildDeviceVisualTooltip(const DeviceEntry& device) {
    const DeviceVisualDecision decision = DetectDeviceVisualDecision(device);
    return QString::fromUtf8(u8"Иконка: %1\nПричина: %2")
        .arg(DeviceVisualTypeLabel(decision.type), decision.reason);
}

QColor DeviceTypeAccentColor(DeviceVisualType type) {
    switch (type) {
    case DeviceVisualType::Headphones:
        return QColor(QStringLiteral("#63B9FF"));
    case DeviceVisualType::Gamepad:
        return QColor(QStringLiteral("#67DF93"));
    case DeviceVisualType::Phone:
        return QColor(QStringLiteral("#59D5C6"));
    case DeviceVisualType::Keyboard:
        return QColor(QStringLiteral("#F1C86A"));
    case DeviceVisualType::Mouse:
        return QColor(QStringLiteral("#FFB56A"));
    case DeviceVisualType::Speaker:
        return QColor(QStringLiteral("#F28F8F"));
    case DeviceVisualType::Watch:
        return QColor(QStringLiteral("#9BCB7A"));
    case DeviceVisualType::Computer:
        return QColor(QStringLiteral("#8EB6FF"));
    case DeviceVisualType::Laptop:
        return QColor(QStringLiteral("#A3C6FF"));
    case DeviceVisualType::Generic:
    default:
        return QColor(QStringLiteral("#DDE5EF"));
    }
}

QString DeviceTypeIconPath(DeviceVisualType type) {
    switch (type) {
    case DeviceVisualType::Headphones:
        return QStringLiteral(":/icons/headphones.svg");
    case DeviceVisualType::Gamepad:
        return QStringLiteral(":/icons/gamepad.svg");
    case DeviceVisualType::Phone:
        return QStringLiteral(":/icons/phone.svg");
    case DeviceVisualType::Keyboard:
        return QStringLiteral(":/icons/keyboard.svg");
    case DeviceVisualType::Mouse:
        return QStringLiteral(":/icons/mouse.svg");
    case DeviceVisualType::Speaker:
        return QStringLiteral(":/icons/speaker.svg");
    case DeviceVisualType::Watch:
        return QStringLiteral(":/icons/watch.svg");
    case DeviceVisualType::Computer:
        return QStringLiteral(":/icons/computer.svg");
    case DeviceVisualType::Laptop:
        return QStringLiteral(":/icons/laptop.svg");
    case DeviceVisualType::Generic:
    default:
        return QStringLiteral(":/icons/generic.svg");
    }
}

QPixmap BuildDeviceTypePixmap(DeviceVisualType type, const QSize& logical_size) {
    const qreal dpr = qApp != nullptr ? qApp->devicePixelRatio() : 1.0;
    const QSize pixel_size(
        std::max(1, qRound(static_cast<qreal>(logical_size.width()) * dpr)),
        std::max(1, qRound(static_cast<qreal>(logical_size.height()) * dpr)));

    QPixmap pixmap(pixel_size);
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(dpr);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRectF badge_rect(0.5, 0.5, logical_size.width() - 1.0, logical_size.height() - 1.0);
    const QColor accent = DeviceTypeAccentColor(type);
    const QColor badge_fill(QStringLiteral("#2B2F36"));

    painter.setPen(Qt::NoPen);
    painter.setBrush(badge_fill);
    painter.drawRoundedRect(badge_rect, 11.0, 11.0);

    QPen border_pen(accent);
    border_pen.setWidthF(1.0);
    painter.setPen(border_pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(badge_rect.adjusted(0.25, 0.25, -0.25, -0.25), 11.0, 11.0);

    const QRectF content_rect = badge_rect.adjusted(
        logical_size.width() * 0.18,
        logical_size.height() * 0.18,
        -logical_size.width() * 0.18,
        -logical_size.height() * 0.18);
    const QString icon_path = DeviceTypeIconPath(type);
    QSvgRenderer renderer(icon_path);
    if (renderer.isValid()) {
        renderer.render(&painter, content_rect);
    }

    return pixmap;
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
        const std::string grouping_key = DeviceGroupingKey(device_id);

        std::size_t index = 0;
        const auto found = index_by_id.find(grouping_key);
        if (found == index_by_id.end()) {
            index = grouped.size();
            index_by_id.emplace(grouping_key, index);

            DeviceEntry device;
            device.device_id = device_id;
            device.device_name = device_name;
            device.device_mode = item.device_mode;
            device.device_submode = item.device_submode;
            device.bluetooth_le_appearance = item.bluetooth_le_appearance;
            device.bluetooth_cod_major = item.bluetooth_cod_major;
            device.bluetooth_cod_minor = item.bluetooth_cod_minor;
            device.device_categories = item.device_categories;
            device.is_connected = item.is_connected;
            grouped.push_back(std::move(device));
        } else {
            index = found->second;
            if (grouped[index].device_name == "Unknown device" && device_name != "Unknown device") {
                grouped[index].device_name = device_name;
            }
            if (!grouped[index].device_mode.has_value() && item.device_mode.has_value()) {
                grouped[index].device_mode = item.device_mode;
            }
            if (!grouped[index].device_submode.has_value() && item.device_submode.has_value()) {
                grouped[index].device_submode = item.device_submode;
            }
            if (!grouped[index].bluetooth_le_appearance.has_value() && item.bluetooth_le_appearance.has_value()) {
                grouped[index].bluetooth_le_appearance = item.bluetooth_le_appearance;
            }
            if (!grouped[index].bluetooth_cod_major.has_value() && item.bluetooth_cod_major.has_value()) {
                grouped[index].bluetooth_cod_major = item.bluetooth_cod_major;
            }
            if (!grouped[index].bluetooth_cod_minor.has_value() && item.bluetooth_cod_minor.has_value()) {
                grouped[index].bluetooth_cod_minor = item.bluetooth_cod_minor;
            }
            for (const auto& category : item.device_categories) {
                const auto it = std::find(grouped[index].device_categories.begin(), grouped[index].device_categories.end(), category);
                if (it == grouped[index].device_categories.end()) {
                    grouped[index].device_categories.push_back(category);
                }
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
        } else if (ComponentQualityScore(incoming) >= ComponentQualityScore(*existing)) {
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
        if (connected_queue || device.is_connected == connected_queue) {
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
        if ((connected_queue || device.is_connected == connected_queue) && !ordered_ids.contains(device.device_id)) {
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
    append_queue(connected_order, false);
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
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_StyledBackground, false);
        setAutoFillBackground(false);
        setFocusPolicy(Qt::NoFocus);
        setWindowOpacity(1.0);

        setStyleSheet(R"(
QFrame#lowBatteryToast {
    background: transparent;
    border: none;
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

   protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QRectF frame_rect = rect();
        frame_rect.adjust(0.5, 0.5, -0.5, -0.5);
        QPainterPath path;
        path.addRoundedRect(frame_rect, 12.0, 12.0);
        painter.fillPath(path, QColor(QStringLiteral("#3B3E44")));
        painter.setPen(QPen(QColor(255, 255, 255, 23), 1.0));
        painter.drawPath(path);
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
    : QWidget(parent),
      provider_(std::move(provider)),
      noise_control_provider_(provider_ != nullptr ? provider_->GetNoiseControlProvider() : nullptr) {
    setObjectName(QStringLiteral("trayPanelWindow"));
    setWindowTitle(QStringLiteral("ChargeViewer"));
    setWindowFlag(Qt::Tool, true);
    setWindowFlag(Qt::WindowStaysOnTopHint, true);
    setWindowFlag(Qt::FramelessWindowHint, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAutoFillBackground(false);
    setFixedWidth(kDefaultWindowWidth);

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
    color: #F8FAFC;
    background: transparent;
    border: none;
    padding: 4px 10px;
    font-size: 11px;
    font-weight: 600;
}
QPushButton#topButton:hover {
    background: transparent;
}
QPushButton#topButton:disabled {
    color: #9CA3AF;
    background: transparent;
}
QLabel#summaryLabel {
    color: #D1D5DB;
    font-size: 10px;
}
QFrame#deviceRow {
    background: transparent;
    border: none;
}
QFrame#deviceRow[activeState="inactive"] {
    background: transparent;
}
QFrame#deviceRow:hover {
    background: transparent;
}
QFrame#deviceRow[dragOver="true"] {
    background: transparent;
}
QLabel#deviceIcon {
    background: transparent;
    border: none;
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
    color: #E7ECF6;
    background: transparent;
    border: none;
    padding: 1px 7px;
    font-size: 12px;
    font-weight: 700;
}
QLabel#percentChip[levelState="low"] {
    background: transparent;
    color: #FFD0D5;
}
QLabel#percentChip[activeState="inactive"] {
    background: transparent;
    color: #B7BFCD;
}
QProgressBar#deviceProgress {
    background: transparent;
    border: none;
    min-height: 10px;
    max-height: 10px;
}
QProgressBar#deviceProgress::chunk {
    background: transparent;
}
QProgressBar#deviceProgress[levelState="ok"]::chunk {
    background: transparent;
}
QProgressBar#deviceProgress[levelState="warn"]::chunk {
    background: transparent;
}
QProgressBar#deviceProgress[levelState="low"]::chunk {
    background: transparent;
}
QProgressBar#deviceProgress[levelState="na"]::chunk {
    background: transparent;
}
QProgressBar#deviceProgress[activeState="inactive"] {
    background: transparent;
}
QProgressBar#deviceProgress[activeState="inactive"]::chunk {
    background: transparent;
}
QToolButton#inlineMenuButton {
    background: transparent;
    color: #A7B0C0;
    border: none;
    min-width: 28px;
    max-width: 28px;
    min-height: 28px;
    max-height: 28px;
    font-size: 15px;
    font-weight: 600;
}
QToolButton#inlineMenuButton:hover {
    background: transparent;
    color: #D6DCE7;
}
QToolButton#settingsButton {
    background: transparent;
    color: #E7EDF8;
    border: none;
    min-width: 30px;
    max-width: 30px;
    min-height: 30px;
    max-height: 30px;
    font-size: 14px;
    font-weight: 700;
}
QToolButton#settingsButton:hover {
    background: transparent;
}
QFrame#settingsPanel {
    background: transparent;
    border: none;
}
QLabel#settingsLabel {
    color: #D8DEE9;
    font-size: 11px;
    font-weight: 500;
}
QSpinBox#settingsSpinBox {
    background: transparent;
    color: #F2F5FB;
    border: none;
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

    auto* title_label = new QLabel(QStringLiteral("ChargeViewer"), this);
    title_label->setObjectName(QStringLiteral("titleLabel"));

    refresh_button_ = new SmoothPushButton(QString::fromUtf8(u8"\u041E\u0431\u043D\u043E\u0432\u0438\u0442\u044C"), this);
    refresh_button_->setObjectName(QStringLiteral("topButton"));

    show_all_button_ = new SmoothPushButton(QString::fromUtf8(u8"\u041F\u043E\u043A\u0430\u0437\u0430\u0442\u044C "
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

    settings_panel_ = new SmoothFrame(this);
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

    refresh_interval_spinbox_ = new SmoothSpinBox(settings_panel_);
    refresh_interval_spinbox_->setObjectName(QStringLiteral("settingsSpinBox"));
    refresh_interval_spinbox_->setRange(
        kBatteryWindowMinRefreshIntervalSeconds, kBatteryWindowMaxRefreshIntervalSeconds);
    refresh_interval_spinbox_->setSingleStep(1);
    refresh_interval_spinbox_->setSuffix(QString::fromUtf8(u8" с"));

    auto* threshold_row_layout = new QHBoxLayout();
    threshold_row_layout->setContentsMargins(0, 0, 0, 0);
    threshold_row_layout->setSpacing(8);

    auto* threshold_settings_label = new QLabel(
        QString::fromUtf8(u8"\u041F\u043E\u0440\u043E\u0433 \u0443\u0432\u0435\u0434\u043E\u043C\u043B\u0435\u043D\u0438\u044F (%):"),
        settings_panel_);
    threshold_settings_label->setObjectName(QStringLiteral("settingsLabel"));

    low_battery_threshold_spinbox_ = new SmoothSpinBox(settings_panel_);
    low_battery_threshold_spinbox_->setObjectName(QStringLiteral("settingsSpinBox"));
    low_battery_threshold_spinbox_->setRange(
        kBatteryWindowMinLowBatteryThresholdPercent, kBatteryWindowMaxLowBatteryThresholdPercent);
    low_battery_threshold_spinbox_->setSingleStep(1);
    low_battery_threshold_spinbox_->setSuffix(QStringLiteral("%"));

    auto* repeat_row_layout = new QHBoxLayout();
    repeat_row_layout->setContentsMargins(0, 0, 0, 0);
    repeat_row_layout->setSpacing(8);

    auto* repeat_settings_label = new QLabel(
        QString::fromUtf8(u8"\u041F\u043E\u0432\u0442\u043E\u0440 \u0443\u0432\u0435\u0434\u043E\u043C\u043B\u0435\u043D\u0438\u044F (мин):"),
        settings_panel_);
    repeat_settings_label->setObjectName(QStringLiteral("settingsLabel"));

    low_battery_repeat_spinbox_ = new SmoothSpinBox(settings_panel_);
    low_battery_repeat_spinbox_->setObjectName(QStringLiteral("settingsSpinBox"));
    low_battery_repeat_spinbox_->setRange(
        kBatteryWindowMinLowBatteryRepeatMinutes, kBatteryWindowMaxLowBatteryRepeatMinutes);
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

    settings_button_ = new SmoothToolButton(this);
    settings_button_->setObjectName(QStringLiteral("settingsButton"));
    settings_button_->setProperty("smoothKind", QStringLiteral("settings"));
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

    connect(refresh_button_, &QPushButton::clicked, this, [this]() { RefreshBatteryDataFromUser(); });
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
    runtime_timer_ = new QTimer(this);
    bluetooth_refresh_debounce_timer_ = new QTimer(this);
    bluetooth_refresh_debounce_timer_->setSingleShot(true);
    bluetooth_refresh_debounce_timer_->setInterval(250);
    const BatteryWindowPersistedState persisted_state = LoadBatteryWindowPersistedState();
    refresh_interval_ms_ = persisted_state.refresh_interval_ms;
    low_battery_threshold_percent_ = persisted_state.low_battery_threshold_percent;
    low_battery_repeat_minutes_ = persisted_state.low_battery_repeat_minutes;
    refresh_timer_->setInterval(refresh_interval_ms_);
    if (refresh_interval_spinbox_ != nullptr) {
        refresh_interval_spinbox_->blockSignals(true);
        refresh_interval_spinbox_->setValue(
            std::max(kBatteryWindowMinRefreshIntervalSeconds, refresh_interval_ms_ / 1000));
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
    connect(refresh_timer_, &QTimer::timeout, this, [this]() { RefreshBatteryData(false, true); });
    connect(runtime_timer_, &QTimer::timeout, this, [this]() { UpdateRuntimeCountdownLabels(); });
    connect(bluetooth_refresh_debounce_timer_, &QTimer::timeout, this, [this]() {
        if (pending_bluetooth_refresh_device_id_.empty()) {
            RefreshBatteryData(false, true);
            return;
        }
        RefreshBatteryDataForDevice(pending_bluetooth_refresh_device_id_);
        pending_bluetooth_refresh_device_id_.clear();
    });
    refresh_timer_->start();
    runtime_timer_->setInterval(1000);
    runtime_timer_->start();
    UpdateRefreshSettingsTooltip();
    RunLowBatteryNotificationSelfCheck();

    connected_device_order_ = persisted_state.connected_device_order;
    disconnected_device_order_ = persisted_state.disconnected_device_order;

    AdjustWindowHeightForRows(kMaxVisibleRows);
    InitializeTray();
#ifdef BATTERY_MONITOR_WITH_UPDATER
    update_service_ = new UpdateService(this);
    connect(update_service_, &UpdateService::CheckFinished, this,
            [this](bool available, const UpdateManifest& manifest, const QString& error) {
        if (!error.isEmpty()) {
            if (status_label_ != nullptr) {
                status_label_->setText(QString::fromUtf8(u8"Ошибка проверки обновлений: %1").arg(error));
            }
            return;
        }
        if (available) {
            ShowApplicationUpdate(manifest);
        } else if (status_label_ != nullptr) {
            status_label_->setText(QString::fromUtf8(u8"Установлена последняя версия"));
        }
    });
    connect(update_service_, &UpdateService::DownloadProgress, this,
            [this](qint64 received, qint64 total) {
        if (update_dialog_ != nullptr) update_dialog_->SetDownloadProgress(received, total);
    });
    connect(update_service_, &UpdateService::InstallFailed, this, [this](const QString& error) {
        if (update_dialog_ != nullptr) update_dialog_->SetError(error);
    });
    connect(update_service_, &UpdateService::InstallReady, this, [this]() {
        if (update_dialog_ != nullptr) update_dialog_->SetInstalling();
        PrepareForUpdateExit();
    });
    QTimer::singleShot(5000 + QRandomGenerator::global()->bounded(25000), this,
                       [this]() { CheckForApplicationUpdates(false); });
#endif
    StartBluetoothDeviceWatcher();
    RefreshBatteryData(true, false);
}

BatteryWindow::~BatteryWindow() {
    quitting_ = true;
    StopBluetoothDeviceWatcher();
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
#ifdef BATTERY_MONITOR_WITH_UPDATER
    check_updates_action_ = tray_menu_->addAction(
        QString::fromUtf8(u8"Проверить обновления (%1)").arg(QStringLiteral(BATTERY_MONITOR_VERSION)));
    tray_menu_->addSeparator();
#endif
    quit_action_ = tray_menu_->addAction(QString::fromUtf8(u8"\u0412\u044B\u0445\u043E\u0434"));

    connect(toggle_window_action_, &QAction::triggered, this, [this]() {
        if (isVisible()) {
            HideWindowToTray();
        } else {
            ShowWindowFromTray();
        }
    });
    connect(refresh_action_, &QAction::triggered, this, [this]() { RefreshBatteryDataFromUser(); });
    connect(reset_hidden_action_, &QAction::triggered, this, [this]() { ResetHiddenDevices(); });
#ifdef BATTERY_MONITOR_WITH_UPDATER
    connect(check_updates_action_, &QAction::triggered, this,
            [this]() { CheckForApplicationUpdates(true); });
#endif
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
    tray_icon_->setToolTip(QStringLiteral("ChargeViewer"));
    tray_icon_->show();

    UpdateToggleActionText();
}

void BatteryWindow::CheckForApplicationUpdates(bool user_initiated) {
#ifdef BATTERY_MONITOR_WITH_UPDATER
    if (update_service_ == nullptr) {
        return;
    }
    if (user_initiated && status_label_ != nullptr) {
        status_label_->setText(QString::fromUtf8(u8"Проверка обновлений…"));
    }
    update_service_->CheckForUpdates(user_initiated);
#else
    (void)user_initiated;
#endif
}

void BatteryWindow::ShowApplicationUpdate(const UpdateManifest& manifest) {
#ifdef BATTERY_MONITOR_WITH_UPDATER
    if (update_dialog_ != nullptr) {
        update_dialog_->show();
        update_dialog_->raise();
        update_dialog_->activateWindow();
        return;
    }
    auto* dialog = new UpdateDialog(manifest, this);
    update_dialog_ = dialog;
    connect(dialog, &UpdateDialog::InstallRequested, this, [this, manifest]() {
        if (update_service_ != nullptr) update_service_->DownloadAndInstall(manifest);
    });
    connect(dialog, &QObject::destroyed, this, [this]() { update_dialog_ = nullptr; });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
#else
    (void)manifest;
#endif
}

void BatteryWindow::PrepareForUpdateExit() {
    quitting_ = true;
    if (refresh_timer_ != nullptr) refresh_timer_->stop();
    if (runtime_timer_ != nullptr) runtime_timer_->stop();
    if (bluetooth_refresh_debounce_timer_ != nullptr) bluetooth_refresh_debounce_timer_->stop();
    StopBluetoothDeviceWatcher();
    if (tray_icon_ != nullptr) tray_icon_->hide();
    close();
    if (qApp != nullptr) qApp->quit();
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
    QString tooltip = QString::fromUtf8(u8"ChargeViewer\n"
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

    QIcon tray_icon = windowIcon();
    if (!ordered.empty()) {
        const auto primary = ComputePrimaryBattery(ordered.front());
        if (primary.level.has_value()) {
            tray_icon = BuildTrayLevelIcon(static_cast<int>(*primary.level));
        }
    }

    tray_icon_->setIcon(tray_icon);
    tray_icon_->setToolTip(tooltip);
}

void BatteryWindow::NotifyLowBatteryIfNeeded(const std::vector<DeviceBatteryInfo>& devices) {
    std::unordered_map<std::string, LowBatteryDeviceState> device_states;
    const std::uint8_t threshold_percent =
        static_cast<std::uint8_t>(
            ClampBatteryWindowLowBatteryThresholdPercent(low_battery_threshold_percent_));
    const std::int64_t repeat_interval_ms =
        static_cast<std::int64_t>(
            ClampBatteryWindowLowBatteryRepeatMinutes(low_battery_repeat_minutes_)) *
        60LL * 1000LL;
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

void BatteryWindow::RecordBatteryHistory(const std::vector<DeviceBatteryInfo>& devices) {
    history_store_.RecordSnapshot(devices);

    for (auto it = history_dialogs_.begin(); it != history_dialogs_.end();) {
        if (it->second.isNull()) {
            it = history_dialogs_.erase(it);
            continue;
        }

        it->second->SetHistory(history_store_.LoadHistory(ToQString(it->first)));
        ++it;
    }

    for (auto it = stats_dialogs_.begin(); it != stats_dialogs_.end();) {
        if (it->second.isNull()) {
            it = stats_dialogs_.erase(it);
            continue;
        }

        it->second->SetHistory(history_store_.LoadHistory(ToQString(it->first)));
        ++it;
    }

    for (auto it = diagnostics_dialogs_.begin(); it != diagnostics_dialogs_.end();) {
        if (it->second.isNull()) {
            it = diagnostics_dialogs_.erase(it);
            continue;
        }
        ++it;
    }
}

void BatteryWindow::ShowBatteryHistory(const std::string& device_id, const std::string& fallback_name) {
    if (device_id.empty()) {
        return;
    }

    BatteryHistoryData history = history_store_.LoadHistory(ToQString(device_id));
    if (history.device_name.trimmed().isEmpty()) {
        history.device_name = ToQString(fallback_name);
    }
    const auto runtime_deadline_it = runtime_deadline_ms_by_device_.find(device_id);
    const std::optional<qint64> runtime_deadline_ms =
        runtime_deadline_it != runtime_deadline_ms_by_device_.end()
            ? std::optional<qint64>(runtime_deadline_it->second)
            : std::nullopt;
    QHash<QString, qint64> component_runtime_deadlines_ms;
    const auto component_deadline_it = runtime_deadline_ms_by_component_.find(device_id);
    if (component_deadline_it != runtime_deadline_ms_by_component_.end()) {
        for (const auto& [component_key, deadline_ms] : component_deadline_it->second) {
            component_runtime_deadlines_ms.insert(ToQString(component_key), deadline_ms);
        }
    }

    const auto existing_dialog = history_dialogs_.find(device_id);
    if (existing_dialog != history_dialogs_.end() && !existing_dialog->second.isNull()) {
        existing_dialog->second->SetHistory(std::move(history));
        existing_dialog->second->SetRuntimeDeadline(runtime_deadline_ms);
        existing_dialog->second->SetComponentRuntimeDeadlines(component_runtime_deadlines_ms);
        existing_dialog->second->show();
        existing_dialog->second->raise();
        existing_dialog->second->activateWindow();
        return;
    }

    auto* dialog = new BatteryHistoryDialog(std::move(history), this);
    dialog->SetRuntimeDeadline(runtime_deadline_ms);
    dialog->SetComponentRuntimeDeadlines(component_runtime_deadlines_ms);
    history_dialogs_[device_id] = dialog;
    connect(dialog, &QObject::destroyed, this, [this, device_id]() { history_dialogs_.erase(device_id); });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void BatteryWindow::ShowBatteryStats(const std::string& device_id, const std::string& fallback_name) {
    if (device_id.empty()) {
        return;
    }

    BatteryHistoryData history = history_store_.LoadHistory(ToQString(device_id));
    if (history.device_name.trimmed().isEmpty()) {
        history.device_name = ToQString(fallback_name);
    }

    const auto existing_dialog = stats_dialogs_.find(device_id);
    if (existing_dialog != stats_dialogs_.end() && !existing_dialog->second.isNull()) {
        existing_dialog->second->SetHistory(std::move(history));
        existing_dialog->second->show();
        existing_dialog->second->raise();
        existing_dialog->second->activateWindow();
        return;
    }

    auto* dialog = new BatteryStatsDialog(std::move(history), this);
    stats_dialogs_[device_id] = dialog;
    connect(dialog, &QObject::destroyed, this, [this, device_id]() { stats_dialogs_.erase(device_id); });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void BatteryWindow::ShowDeviceDiagnostics(const std::string& device_id) {
    if (device_id.empty()) {
        return;
    }

    const auto existing_dialog = diagnostics_dialogs_.find(device_id);
    if (existing_dialog != diagnostics_dialogs_.end() && !existing_dialog->second.isNull()) {
        existing_dialog->second->show();
        existing_dialog->second->raise();
        existing_dialog->second->activateWindow();
        return;
    }

    std::vector<DeviceBatteryInfo> entries;
    for (const auto& entry : last_devices_snapshot_) {
        if (DeviceIdsReferToSameBluetoothDevice(entry.device_id, device_id)) {
            entries.push_back(entry);
        }
    }
    if (entries.empty()) {
        return;
    }

    auto* dialog = new DeviceDiagnosticsDialog(std::move(entries), this);
    diagnostics_dialogs_[device_id] = dialog;
    connect(dialog, &QObject::destroyed, this, [this, device_id]() { diagnostics_dialogs_.erase(device_id); });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
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
    const int seconds = std::max(kBatteryWindowMinRefreshIntervalSeconds, refresh_interval_ms_ / 1000);
    const int threshold = ClampBatteryWindowLowBatteryThresholdPercent(low_battery_threshold_percent_);
    const int repeat_minutes = ClampBatteryWindowLowBatteryRepeatMinutes(low_battery_repeat_minutes_);
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
    refresh_interval_ms_ = ClampBatteryWindowRefreshIntervalMs(seconds * 1000);
    if (refresh_timer_ != nullptr) {
        refresh_timer_->setInterval(refresh_interval_ms_);
    }
    SaveBatteryWindowRefreshIntervalMs(refresh_interval_ms_);
    UpdateRefreshSettingsTooltip();

    if (!announce_status || status_label_ == nullptr) {
        return;
    }

    status_label_->setText(QString::fromUtf8(u8"\u0418\u043D\u0442\u0435\u0440\u0432\u0430\u043B "
                                             u8"\u0430\u0432\u0442\u043E\u043E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u0438\u044F: %1 \u0441\u0435\u043A")
                              .arg(refresh_interval_ms_ / 1000));
}

void BatteryWindow::ApplyLowBatteryThresholdPercent(int percent, bool announce_status) {
    const int new_threshold = ClampBatteryWindowLowBatteryThresholdPercent(percent);
    const bool threshold_changed = new_threshold != low_battery_threshold_percent_;
    low_battery_threshold_percent_ = new_threshold;
    SaveBatteryWindowLowBatteryThresholdPercent(low_battery_threshold_percent_);
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
    low_battery_repeat_minutes_ = ClampBatteryWindowLowBatteryRepeatMinutes(minutes);
    SaveBatteryWindowLowBatteryRepeatMinutes(low_battery_repeat_minutes_);
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
            refresh_interval_spinbox_->setValue(
                std::max(kBatteryWindowMinRefreshIntervalSeconds, refresh_interval_ms_ / 1000));
            refresh_interval_spinbox_->blockSignals(false);
        }
        if (low_battery_threshold_spinbox_ != nullptr) {
            low_battery_threshold_spinbox_->blockSignals(true);
            low_battery_threshold_spinbox_->setValue(
                ClampBatteryWindowLowBatteryThresholdPercent(low_battery_threshold_percent_));
            low_battery_threshold_spinbox_->blockSignals(false);
        }
        if (low_battery_repeat_spinbox_ != nullptr) {
            low_battery_repeat_spinbox_->blockSignals(true);
            low_battery_repeat_spinbox_->setValue(
                ClampBatteryWindowLowBatteryRepeatMinutes(low_battery_repeat_minutes_));
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
        const bool next_include_disconnected = pending_include_disconnected_;
        const bool next_preserve_disconnected_snapshot = pending_preserve_disconnected_snapshot_;
        pending_include_disconnected_ = false;
        pending_preserve_disconnected_snapshot_ = true;
        QMetaObject::invokeMethod(
            this,
            [this, next_include_disconnected, next_preserve_disconnected_snapshot]() {
                RefreshBatteryData(next_include_disconnected, next_preserve_disconnected_snapshot);
            },
            Qt::QueuedConnection);
    }
}

void BatteryWindow::StartBluetoothDeviceWatcher() {
#ifdef _WIN32
    try {
        using winrt::Windows::Devices::Bluetooth::BluetoothConnectionStatus;
        using winrt::Windows::Devices::Bluetooth::BluetoothDevice;
        using winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
        using winrt::Windows::Devices::Enumeration::DeviceInformation;

        auto attach_watcher = [this](auto* watcher,
                                     winrt::event_token* added_token,
                                     winrt::event_token* updated_token,
                                     winrt::event_token* removed_token,
                                     const winrt::hstring& selector,
                                     const char* transport) {
            *watcher = DeviceInformation::CreateWatcher(selector);
            QPointer<BatteryWindow> self(this);
            *added_token = watcher->Added([self, transport](auto&&, const auto& info) {
                if (self == nullptr) {
                    return;
                }
                WindowsBatteryProviderEventLog("bluetooth event=connected transport=" + std::string(transport) +
                                               " id='" + winrt::to_string(info.Id()) + "'");
                self->ScheduleBluetoothDeviceRefresh(winrt::to_string(info.Id()), true);
            });
            *updated_token = watcher->Updated([self, transport](auto&&, const auto& info) {
                if (self == nullptr) {
                    return;
                }
                WindowsBatteryProviderEventLog("bluetooth event=updated-connected transport=" + std::string(transport) +
                                               " id='" + winrt::to_string(info.Id()) + "'");
                self->ScheduleBluetoothDeviceRefresh(winrt::to_string(info.Id()), true);
            });
            *removed_token = watcher->Removed([self, transport](auto&&, const auto& info) {
                if (self == nullptr) {
                    return;
                }
                WindowsBatteryProviderEventLog("bluetooth event=disconnected transport=" + std::string(transport) +
                                               " id='" + winrt::to_string(info.Id()) + "'");
                self->ScheduleBluetoothDeviceRefresh(winrt::to_string(info.Id()), false);
            });
            watcher->Start();
            WindowsBatteryProviderEventLog("bluetooth watcher started transport=" + std::string(transport));
        };

        attach_watcher(&bluetooth_classic_watcher_,
                       &bluetooth_classic_added_token_,
                       &bluetooth_classic_updated_token_,
                       &bluetooth_classic_removed_token_,
                       BluetoothDevice::GetDeviceSelectorFromConnectionStatus(BluetoothConnectionStatus::Connected),
                       "classic");
        attach_watcher(&bluetooth_le_watcher_,
                       &bluetooth_le_added_token_,
                       &bluetooth_le_updated_token_,
                       &bluetooth_le_removed_token_,
                       BluetoothLEDevice::GetDeviceSelectorFromConnectionStatus(BluetoothConnectionStatus::Connected),
                       "ble");
    } catch (...) {
        bluetooth_classic_watcher_ = nullptr;
        bluetooth_le_watcher_ = nullptr;
    }
#endif
}

void BatteryWindow::StopBluetoothDeviceWatcher() {
#ifdef _WIN32
    auto stop_watcher = [](auto* watcher,
                           winrt::event_token added_token,
                           winrt::event_token updated_token,
                           winrt::event_token removed_token) {
        if (*watcher == nullptr) {
            return;
        }
        try {
            watcher->Added(added_token);
            watcher->Updated(updated_token);
            watcher->Removed(removed_token);
            watcher->Stop();
        } catch (...) {
        }
        *watcher = nullptr;
    };

    stop_watcher(&bluetooth_classic_watcher_,
                 bluetooth_classic_added_token_,
                 bluetooth_classic_updated_token_,
                 bluetooth_classic_removed_token_);
    stop_watcher(&bluetooth_le_watcher_,
                 bluetooth_le_added_token_,
                 bluetooth_le_updated_token_,
                 bluetooth_le_removed_token_);
#endif
}

void BatteryWindow::ScheduleBluetoothDeviceRefresh(const std::string& changed_device_id, bool connected) {
    QMetaObject::invokeMethod(
        this,
        [this, changed_device_id, connected]() {
            if (quitting_) {
                return;
            }

            provider_->NotifyDeviceConnectionChanged(changed_device_id, connected);

            const bool applied_locally = ApplyBluetoothDeviceConnectionChange(changed_device_id, connected);
            if (applied_locally && !connected) {
                return;
            }

            if (bluetooth_refresh_debounce_timer_ == nullptr) {
                RefreshBatteryDataForDevice(changed_device_id);
                return;
            }

            if (pending_bluetooth_refresh_device_id_.empty()) {
                pending_bluetooth_refresh_device_id_ = changed_device_id;
            } else if (!DeviceIdsReferToSameBluetoothDevice(pending_bluetooth_refresh_device_id_, changed_device_id)) {
                pending_bluetooth_refresh_device_id_.clear();
            }
            bluetooth_refresh_debounce_timer_->start();
        },
        Qt::QueuedConnection);
}

bool BatteryWindow::ApplyBluetoothDeviceConnectionChange(const std::string& changed_device_id, bool connected) {
    if (changed_device_id.empty() || last_devices_snapshot_.empty()) {
        return false;
    }

    auto devices = last_devices_snapshot_;
    bool changed = false;
    for (auto& device : devices) {
        if (!DeviceIdsReferToSameBluetoothDevice(device.device_id, changed_device_id)) {
            continue;
        }
        device.is_connected = connected;
        if (!connected) {
            device.battery_level_percent.reset();
            device.device_mode.reset();
            device.device_submode.reset();
            device.is_cached = false;
            last_live_update_.erase(device.device_id);
            runtime_deadline_ms_by_device_.erase(device.device_id);
            runtime_state_key_by_device_.erase(device.device_id);
            runtime_deadline_ms_by_component_.erase(device.device_id);
            runtime_state_key_by_component_.erase(device.device_id);
        }
        changed = true;
    }

    if (!changed) {
        return false;
    }

    last_devices_snapshot_ = std::move(devices);
    PopulateDeviceCards(last_devices_snapshot_);
    UpdateTrayTooltip(last_devices_snapshot_);
    if (status_label_ != nullptr) {
        const auto now = QDateTime::currentDateTime();
        status_label_->setText(
            QString::fromUtf8(u8"\u041E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u043E \u0432 %1")
                .arg(now.toString(QStringLiteral("HH:mm:ss"))));
    }
    return true;
}

void BatteryWindow::RefreshBatteryDataForDevice(const std::string& device_id, bool force_live_refresh) {
    const std::uint64_t refresh_id = NextUiRefreshId();
    UiDebugLog("refresh#" + std::to_string(refresh_id) + " targeted requested target='" + device_id + "'");
    if (device_id.empty()) {
        UiDebugLog("refresh#" + std::to_string(refresh_id) + " targeted empty target -> full refresh");
        RefreshBatteryData(false, true, force_live_refresh);
        return;
    }

    if (drag_in_progress_) {
        UiDebugLog("refresh#" + std::to_string(refresh_id) + " targeted deferred because drag_in_progress target='" + device_id + "'");
        refresh_pending_ = true;
        pending_preserve_disconnected_snapshot_ = true;
        status_label_->setText(QString::fromUtf8(u8"\u041E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u0438\u0435 "
                                                 u8"\u043E\u0442\u043B\u043E\u0436\u0435\u043D\u043E: "
                                                 u8"\u0438\u0434\u0451\u0442 \u043F\u0435\u0440\u0435\u0442\u0430\u0441\u043A\u0438\u0432\u0430\u043D\u0438\u0435"));
        return;
    }

    if (refresh_in_progress_.load(std::memory_order_acquire)) {
        UiDebugLog("refresh#" + std::to_string(refresh_id) + " targeted deferred because refresh_in_progress target='" + device_id + "'");
        refresh_pending_ = true;
        pending_preserve_disconnected_snapshot_ = true;
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

    refresh_worker_ = std::thread([this, device_id, force_live_refresh, refresh_id]() {
        RefreshTaskResult result;
        try {
            BatteryQueryOptions query_options;
            query_options.include_disconnected = false;
            query_options.force_live_refresh = force_live_refresh;
            query_options.target_device_id = device_id;
            UiDebugLog("refresh#" + std::to_string(refresh_id) +
                       " provider query targeted include_disconnected=false target='" + device_id + "'");
            result.devices = provider_->GetDevicesBattery(query_options);
            UiDebugLogDevices("refresh#" + std::to_string(refresh_id) + " provider result targeted", result.devices);
#ifdef _WIN32
        } catch (const winrt::hresult_error& error) {
            result.error_text = QString::fromUtf8(u8"\u041E\u0448\u0438\u0431\u043A\u0430: %1").arg(FormatWinRtError(error));
            result.is_bluetooth_stack_error = true;
            UiDebugLog("refresh#" + std::to_string(refresh_id) + " provider targeted WinRT error='" +
                       result.error_text.toStdString() + "'");
#endif
        } catch (const std::exception& ex) {
            result.error_text = QString::fromUtf8(u8"\u041E\u0448\u0438\u0431\u043A\u0430: %1").arg(ToQString(FormatError(ex)));
            UiDebugLog("refresh#" + std::to_string(refresh_id) + " provider targeted std error='" +
                       result.error_text.toStdString() + "'");
        } catch (...) {
            result.error_text = QString::fromUtf8(u8"\u041E\u0448\u0438\u0431\u043A\u0430: \u043D\u0435\u0438\u0437\u0432\u0435\u0441\u0442\u043D\u043E\u0435 \u0438\u0441\u043A\u043B\u044E\u0447\u0435\u043D\u0438\u0435");
            UiDebugLog("refresh#" + std::to_string(refresh_id) + " provider targeted unknown error");
        }

        QMetaObject::invokeMethod(
            this,
            [this, device_id, refresh_id, result = std::move(result)]() mutable {
                refresh_in_progress_.store(false, std::memory_order_release);

                if (quitting_) {
                    UiDebugLog("refresh#" + std::to_string(refresh_id) + " targeted ignored because quitting");
                    return;
                }

                if (result.error_text.isEmpty()) {
                    if (result.devices.empty()) {
                        UiDebugLog("refresh#" + std::to_string(refresh_id) + " targeted empty result -> full refresh");
                        RefreshBatteryData(false, true);
                        return;
                    }

                    UiDebugLogDevices("refresh#" + std::to_string(refresh_id) + " snapshot before targeted merge",
                                      last_devices_snapshot_);
                    auto merged_devices = last_devices_snapshot_;
                    merged_devices.erase(
                        std::remove_if(
                            merged_devices.begin(),
                            merged_devices.end(),
                            [&](const DeviceBatteryInfo& existing) {
                                if (DeviceIdsReferToSameBluetoothDevice(existing.device_id, device_id)) {
                                    return true;
                                }
                                return std::any_of(
                                    result.devices.begin(), result.devices.end(),
                                    [&](const DeviceBatteryInfo& incoming) {
                                        return DeviceIdsReferToSameBluetoothDevice(existing.device_id, incoming.device_id);
                                    });
                            }),
                        merged_devices.end());
                    merged_devices.insert(merged_devices.end(), result.devices.begin(), result.devices.end());
                    NotifyLowBatteryIfNeeded(result.devices);
                    RecordBatteryHistory(result.devices);
                    last_devices_snapshot_ = std::move(merged_devices);
                    UiDebugLogDevices("refresh#" + std::to_string(refresh_id) + " snapshot after targeted merge",
                                      last_devices_snapshot_);
                    PopulateDeviceCards(last_devices_snapshot_);
                    UpdateTrayTooltip(last_devices_snapshot_);

                    const auto now = QDateTime::currentDateTime();
                    status_label_->setText(
                        QString::fromUtf8(u8"\u041E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u043E \u0432 %1")
                            .arg(now.toString(QStringLiteral("HH:mm:ss"))));
                } else {
                    UiDebugLog("refresh#" + std::to_string(refresh_id) + " targeted ui error='" +
                               result.error_text.toStdString() + "'");
                    status_label_->setText(result.error_text);
                }

                refresh_button_->setEnabled(true);
                show_all_button_->setEnabled(!hidden_device_ids_.empty());
                UpdateToggleActionText();

                if (refresh_pending_) {
                    const bool next_include_disconnected = pending_include_disconnected_;
                    const bool next_preserve_disconnected_snapshot = pending_preserve_disconnected_snapshot_;
                    refresh_pending_ = false;
                    pending_include_disconnected_ = false;
                    pending_preserve_disconnected_snapshot_ = true;
                    RefreshBatteryData(next_include_disconnected, next_preserve_disconnected_snapshot);
                }
            },
            Qt::QueuedConnection);
    });
}

void BatteryWindow::ApplyNoiseControlMode(const std::string& device_id, NoiseControlMode mode) {
    auto* noise_provider = noise_control_provider_;
    if (noise_provider == nullptr) {
        if (status_label_ != nullptr) {
            status_label_->setText(QString::fromUtf8(u8"\u0423\u043F\u0440\u0430\u0432\u043B\u0435\u043D\u0438\u0435 "
                                                     u8"\u0440\u0435\u0436\u0438\u043C\u043E\u043C "
                                                     u8"\u043D\u0435 \u043F\u043E\u0434\u0434\u0435\u0440\u0436\u0438\u0432\u0430\u0435\u0442\u0441\u044F"));
        }
        return;
    }

    if (refresh_in_progress_.load(std::memory_order_acquire)) {
        return;
    }

    if (refresh_worker_.joinable()) {
        refresh_worker_.join();
    }
    refresh_in_progress_.store(true, std::memory_order_release);
    refresh_button_->setEnabled(false);
    show_all_button_->setEnabled(false);
    status_label_->setText(QString::fromUtf8(u8"\u041F\u0435\u0440\u0435\u043A\u043B\u044E\u0447\u0435\u043D\u0438\u0435 "
                                             u8"\u0440\u0435\u0436\u0438\u043C\u0430..."));

    refresh_worker_ = std::thread([this, device_id, mode]() {
        bool ok = false;
        std::string error_text;
        try {
            if (auto* provider = noise_control_provider_; provider != nullptr) {
                ok = provider->SetNoiseControlMode(device_id, mode);
            }
            if (!ok) {
                error_text = "failed";
            }
        } catch (const std::exception& ex) {
            error_text = FormatError(ex);
        } catch (...) {
            error_text = "unknown";
        }

        QMetaObject::invokeMethod(
            this,
            [this, ok, error_text = std::move(error_text)]() {
                refresh_in_progress_.store(false, std::memory_order_release);
                refresh_button_->setEnabled(true);
                show_all_button_->setEnabled(!hidden_device_ids_.empty());
                if (quitting_) {
                    return;
                }
                if (ok) {
                    status_label_->setText(QString::fromUtf8(u8"\u0420\u0435\u0436\u0438\u043C "
                                                             u8"\u043F\u0435\u0440\u0435\u043A\u043B\u044E\u0447\u0451\u043D"));
                    RefreshBatteryData();
                } else {
                    status_label_->setText(
                        QString::fromUtf8(u8"\u041D\u0435 \u0443\u0434\u0430\u043B\u043E\u0441\u044C "
                                          u8"\u043F\u0435\u0440\u0435\u043A\u043B\u044E\u0447\u0438\u0442\u044C "
                                          u8"\u0440\u0435\u0436\u0438\u043C"));
                }
            },
            Qt::QueuedConnection);
    });
}

void BatteryWindow::ApplyNoiseSubmode(const std::string& device_id,
                                      NoiseControlMode mode,
                                      const std::string& submode_id) {
    auto* noise_provider = noise_control_provider_;
    if (noise_provider == nullptr || !noise_provider->SupportsNoiseSubmodes(device_id, mode)) {
        if (status_label_ != nullptr) {
            status_label_->setText(
                QString::fromUtf8(u8"\u041F\u043E\u0434\u0440\u0435\u0436\u0438\u043C\u044B "
                                 u8"\u0434\u043B\u044F \u044D\u0442\u043E\u0433\u043E "
                                 u8"\u0443\u0441\u0442\u0440\u043E\u0439\u0441\u0442\u0432\u0430 "
                                 u8"\u043D\u0435 \u043F\u043E\u0434\u0434\u0435\u0440\u0436\u0438\u0432\u0430\u044E\u0442\u0441\u044F"));
        }
        return;
    }

    if (refresh_in_progress_.load(std::memory_order_acquire)) {
        return;
    }

    if (refresh_worker_.joinable()) {
        refresh_worker_.join();
    }
    refresh_in_progress_.store(true, std::memory_order_release);
    refresh_button_->setEnabled(false);
    show_all_button_->setEnabled(false);
    status_label_->setText(QString::fromUtf8(u8"\u041F\u0435\u0440\u0435\u043A\u043B\u044E\u0447\u0435\u043D\u0438\u0435 "
                                             u8"\u043F\u043E\u0434\u0440\u0435\u0436\u0438\u043C\u0430..."));

    refresh_worker_ = std::thread([this, device_id, mode, submode_id]() {
        bool ok = false;
        std::string error_text;
        try {
            if (auto* provider = noise_control_provider_; provider != nullptr) {
                ok = provider->SetNoiseSubmode(device_id, mode, submode_id);
            }
            if (!ok) {
                error_text = "failed";
            }
        } catch (const std::exception& ex) {
            error_text = FormatError(ex);
        } catch (...) {
            error_text = "unknown";
        }

        QMetaObject::invokeMethod(
            this,
            [this, ok, error_text = std::move(error_text)]() {
                refresh_in_progress_.store(false, std::memory_order_release);
                refresh_button_->setEnabled(true);
                show_all_button_->setEnabled(!hidden_device_ids_.empty());
                if (quitting_) {
                    return;
                }
                if (ok) {
                    status_label_->setText(QString::fromUtf8(u8"\u041F\u043E\u0434\u0440\u0435\u0436\u0438\u043C "
                                                             u8"\u043F\u0435\u0440\u0435\u043A\u043B\u044E\u0447\u0451\u043D"));
                    RefreshBatteryData();
                } else {
                    status_label_->setText(
                        QString::fromUtf8(u8"\u041D\u0435 \u0443\u0434\u0430\u043B\u043E\u0441\u044C "
                                          u8"\u043F\u0435\u0440\u0435\u043A\u043B\u044E\u0447\u0438\u0442\u044C "
                                          u8"\u043F\u043E\u0434\u0440\u0435\u0436\u0438\u043C"));
                }
            },
            Qt::QueuedConnection);
    });
}

void BatteryWindow::ShowNoiseSubmodeMenu(QWidget* anchor,
                                         const std::string& device_id,
                                         NoiseControlMode mode,
                                         const std::string& active_submode_id) {
    auto* noise_provider = noise_control_provider_;
    if (anchor == nullptr || noise_provider == nullptr || !noise_provider->SupportsNoiseSubmodes(device_id, mode)) {
        return;
    }

    QMenu menu(anchor);
    const std::string normalized_active_id = NormalizeNoiseControlToken(active_submode_id);
    std::vector<std::pair<std::string, std::string>> submodes = noise_provider->GetNoiseSubmodes(device_id, mode);
    if (submodes.empty() && mode == NoiseControlMode::Transparency) {
        submodes = {
            {"standard", "Прозрачность"},
            {"voice", "Усиление голоса"},
        };
    } else if (submodes.empty() && mode == NoiseControlMode::Anc) {
        submodes = {
            {"balanced", "Баланс"},
            {"weak", "Слабое"},
            {"deep", "Глубокое"},
            {"adaptive", "Адаптивное"},
        };
    } else {
        submodes = noise_provider->GetNoiseSubmodes(device_id, mode);
    }

    for (const auto& [submode_id, submode_label] : submodes) {
        QAction* action = menu.addAction(ToQString(submode_label));
        action->setCheckable(true);
        action->setChecked(NormalizeNoiseControlToken(submode_id) == normalized_active_id);
        connect(action, &QAction::triggered, this,
                [this, device_id, mode, submode_id]() { ApplyNoiseSubmode(device_id, mode, submode_id); });
    }

    if (menu.isEmpty()) {
        return;
    }

    menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
}

void BatteryWindow::RefreshBatteryData(bool include_disconnected,
                                       bool preserve_disconnected_snapshot,
                                       bool force_live_refresh) {
    const std::uint64_t refresh_id = NextUiRefreshId();
    UiDebugLog("refresh#" + std::to_string(refresh_id) +
               " full requested include_disconnected=" + (include_disconnected ? "true" : "false") +
               " preserve_disconnected_snapshot=" + (preserve_disconnected_snapshot ? "true" : "false"));
    if (drag_in_progress_) {
        UiDebugLog("refresh#" + std::to_string(refresh_id) + " full deferred because drag_in_progress");
        refresh_pending_ = true;
        pending_include_disconnected_ = pending_include_disconnected_ || include_disconnected;
        pending_preserve_disconnected_snapshot_ = pending_preserve_disconnected_snapshot_ || preserve_disconnected_snapshot;
        status_label_->setText(QString::fromUtf8(u8"\u041E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u0438\u0435 "
                                                 u8"\u043E\u0442\u043B\u043E\u0436\u0435\u043D\u043E: "
                                                 u8"\u0438\u0434\u0451\u0442 \u043F\u0435\u0440\u0435\u0442\u0430\u0441\u043A\u0438\u0432\u0430\u043D\u0438\u0435"));
        return;
    }

    if (refresh_in_progress_.load(std::memory_order_acquire)) {
        UiDebugLog("refresh#" + std::to_string(refresh_id) + " full deferred because refresh_in_progress");
        refresh_pending_ = true;
        pending_include_disconnected_ = pending_include_disconnected_ || include_disconnected;
        pending_preserve_disconnected_snapshot_ = pending_preserve_disconnected_snapshot_ || preserve_disconnected_snapshot;
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

    refresh_worker_ = std::thread([this, include_disconnected, preserve_disconnected_snapshot, force_live_refresh, refresh_id]() {
        RefreshTaskResult result;
        try {
            BatteryQueryOptions query_options;
            query_options.include_disconnected = include_disconnected;
            query_options.force_live_refresh = force_live_refresh;
            UiDebugLog("refresh#" + std::to_string(refresh_id) +
                       " provider query full include_disconnected=" +
                       (include_disconnected ? "true" : "false") + " target='' preserve_disconnected_snapshot=" +
                       (preserve_disconnected_snapshot ? "true" : "false"));
            result.devices = provider_->GetDevicesBattery(query_options);
            UiDebugLogDevices("refresh#" + std::to_string(refresh_id) + " provider result full", result.devices);
#ifdef _WIN32
        } catch (const winrt::hresult_error& error) {
            result.error_text = QString::fromUtf8(u8"\u041E\u0448\u0438\u0431\u043A\u0430: %1").arg(FormatWinRtError(error));
            result.is_bluetooth_stack_error = true;
            UiDebugLog("refresh#" + std::to_string(refresh_id) + " provider full WinRT error='" +
                       result.error_text.toStdString() + "'");
#endif
        } catch (const std::exception& ex) {
            result.error_text = QString::fromUtf8(u8"\u041E\u0448\u0438\u0431\u043A\u0430: %1").arg(ToQString(FormatError(ex)));
            UiDebugLog("refresh#" + std::to_string(refresh_id) + " provider full std error='" +
                       result.error_text.toStdString() + "'");
        } catch (...) {
            result.error_text = QString::fromUtf8(u8"\u041E\u0448\u0438\u0431\u043A\u0430: \u043D\u0435\u0438\u0437\u0432\u0435\u0441\u0442\u043D\u043E\u0435 \u0438\u0441\u043A\u043B\u044E\u0447\u0435\u043D\u0438\u0435");
            UiDebugLog("refresh#" + std::to_string(refresh_id) + " provider full unknown error");
        }

        QMetaObject::invokeMethod(
            this,
            [this,
             refresh_id,
             result = std::move(result),
             preserve_disconnected_snapshot]() mutable {
                refresh_in_progress_.store(false, std::memory_order_release);

                if (quitting_) {
                    UiDebugLog("refresh#" + std::to_string(refresh_id) + " full ignored because quitting");
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
                    if (preserve_disconnected_snapshot) {
                        UiDebugLogDevices("refresh#" + std::to_string(refresh_id) + " snapshot before preserve merge",
                                          last_devices_snapshot_);
                        std::vector<DeviceBatteryInfo> merged_devices = result.devices;
                        for (const auto& previous : last_devices_snapshot_) {
                            if (previous.is_connected) {
                                continue;
                            }
                            const bool replaced = std::any_of(
                                merged_devices.begin(), merged_devices.end(),
                                [&](const DeviceBatteryInfo& current) {
                                    return DeviceIdsReferToSameBluetoothDevice(current.device_id, previous.device_id);
                                });
                            if (!replaced) {
                                merged_devices.push_back(previous);
                            }
                        }
                        result.devices = std::move(merged_devices);
                        UiDebugLogDevices("refresh#" + std::to_string(refresh_id) + " snapshot after preserve merge",
                                          result.devices);
                    }
                    NotifyLowBatteryIfNeeded(result.devices);
                    RecordBatteryHistory(result.devices);
                    last_devices_snapshot_ = result.devices;
                    UiDebugLogDevices("refresh#" + std::to_string(refresh_id) + " snapshot assigned full",
                                      last_devices_snapshot_);
                    PopulateDeviceCards(result.devices);
                    UpdateTrayTooltip(result.devices);

                    const auto now = QDateTime::currentDateTime();
                    status_label_->setText(
                        QString::fromUtf8(u8"\u041E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u043E \u0432 %1")
                            .arg(now.toString(QStringLiteral("HH:mm:ss"))));
                } else {
                    UiDebugLog("refresh#" + std::to_string(refresh_id) + " full ui error='" +
                               result.error_text.toStdString() + "'");
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
                    const bool next_include_disconnected = pending_include_disconnected_;
                    const bool next_preserve_disconnected_snapshot = pending_preserve_disconnected_snapshot_;
                    refresh_pending_ = false;
                    pending_include_disconnected_ = true;
                    pending_preserve_disconnected_snapshot_ = false;
                    RefreshBatteryData(next_include_disconnected, next_preserve_disconnected_snapshot);
                }
            },
            Qt::QueuedConnection);
    });
}

void BatteryWindow::RefreshBatteryDataFromUser() {
    UiDebugLog("manual refresh requested: stop debounce and force startup-equivalent full refresh");
    if (bluetooth_refresh_debounce_timer_ != nullptr) {
        bluetooth_refresh_debounce_timer_->stop();
    }
    pending_bluetooth_refresh_device_id_.clear();
    RefreshBatteryData(true, false, true);
}

void BatteryWindow::PopulateDeviceCards(const std::vector<DeviceBatteryInfo>& devices) {
    UiDebugLogDevices("render input", devices);
    ClearDeviceCards();

    const auto grouped = GroupDevices(devices, hidden_device_ids_);
    UiDebugLogGroupedDevices("render grouped", grouped);
    SyncOrderQueue(grouped, true, &connected_device_order_);
    SyncOrderQueue(grouped, false, &disconnected_device_order_);
    const auto ordered = ApplyCustomOrder(grouped, connected_device_order_, disconnected_device_order_);
    UiDebugLogGroupedDevices("render ordered", ordered);
    const bool has_noise_control_devices = std::any_of(
        ordered.begin(), ordered.end(),
        [this](const DeviceEntry& device) {
            auto* noise_provider = noise_control_provider_;
            return noise_provider != nullptr &&
                   device.device_mode.has_value() &&
                   noise_provider->SupportsNoiseControl(device.device_id);
        });
    setFixedWidth(has_noise_control_devices ? kNoiseControlWindowWidth : kDefaultWindowWidth);

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
        const bool is_active = IsDeviceConnected(device);
        auto* row = new DraggableDeviceRow(device.device_id, IsDeviceConnected(device), cards_container_);
        row->setObjectName(QStringLiteral("deviceRow"));
        auto* noise_provider = noise_control_provider_;
        const bool supports_noise_control =
            noise_provider != nullptr &&
            is_active &&
            device.device_mode.has_value() &&
            noise_provider->SupportsNoiseControl(device.device_id);
        const int row_height = supports_noise_control ? kNoiseControlRowHeight : kCollapsedRowHeight;
        row->setFixedHeight(row_height);
        row->setMinimumHeight(row_height);
        row->setMaximumHeight(row_height);
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
                    SaveBatteryWindowDeviceOrder(connected_device_order_, disconnected_device_order_);
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

        auto* icon_label = new QLabel(row);
        icon_label->setObjectName(QStringLiteral("deviceIcon"));
        icon_label->setAlignment(Qt::AlignCenter);
        icon_label->setFixedSize(34, 34);
        icon_label->setPixmap(BuildDeviceTypePixmap(DetectDeviceVisualType(device), QSize(34, 34)));
        icon_label->setToolTip(BuildDeviceVisualTooltip(device));

        auto* center_widget = new QWidget(row);
        auto* center_layout = new QVBoxLayout(center_widget);
        center_layout->setContentsMargins(0, 0, 0, 0);
        center_layout->setSpacing(4);

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
        const QString mode_text = BuildDeviceModeText(device);
        QString runtime_text;
        qint64 runtime_deadline_ms = -1;
        if (is_active) {
            const BatteryHistoryData device_history = history_store_.LoadHistory(ToQString(device.device_id));
            const BatteryRuntimeForecast runtime_forecast = EstimateBatteryRuntimeForecast(device_history);
            auto& component_deadlines = runtime_deadline_ms_by_component_[device.device_id];
            auto& component_state_keys = runtime_state_key_by_component_[device.device_id];
            std::unordered_set<std::string> seen_component_keys;

            if (!device_history.samples.isEmpty()) {
                for (auto it = runtime_forecast.by_component.cbegin(); it != runtime_forecast.by_component.cend(); ++it) {
                    if (!it->remaining_ms.has_value()) {
                        continue;
                    }

                    const std::string component_key = it.key().toStdString();
                    seen_component_keys.insert(component_key);

                    const qint64 proposed_deadline_ms = device_history.samples.back().timestamp_ms + *it->remaining_ms;
                    const std::string component_state_key =
                        BuildRuntimeStateKey(device, it.key(), false);
                    const auto component_state_it = component_state_keys.find(component_key);
                    const auto component_deadline_it = component_deadlines.find(component_key);
                    if (component_state_it != component_state_keys.end() &&
                        component_deadline_it != component_deadlines.end() &&
                        component_state_it->second == component_state_key) {
                        component_deadlines[component_key] =
                            ResolveStickyRuntimeDeadline(component_deadline_it->second, proposed_deadline_ms);
                    } else {
                        component_deadlines[component_key] = proposed_deadline_ms;
                    }
                    component_state_keys[component_key] = component_state_key;
                }
            }

            std::vector<std::string> stale_component_keys;
            for (const auto& [component_key, _] : component_deadlines) {
                if (seen_component_keys.find(component_key) == seen_component_keys.end()) {
                    stale_component_keys.push_back(component_key);
                }
            }
            for (const auto& component_key : stale_component_keys) {
                component_deadlines.erase(component_key);
                component_state_keys.erase(component_key);
            }

            QString selected_component_key;
            bool use_pair_forecast = false;
            std::optional<qint64> remaining_ms;
            if (runtime_forecast.pair_remaining_ms.has_value()) {
                use_pair_forecast = true;
                remaining_ms = runtime_forecast.pair_remaining_ms;
            } else {
                const std::array<QString, 4> runtime_order = {
                    QStringLiteral("left"),
                    QStringLiteral("right"),
                    QStringLiteral("main"),
                    QStringLiteral("case"),
                };
                for (const auto& key : runtime_order) {
                    const auto found = runtime_forecast.by_component.find(key);
                    if (found == runtime_forecast.by_component.end() || !found->remaining_ms.has_value()) {
                        continue;
                    }
                    selected_component_key = key;
                    remaining_ms = found->remaining_ms;
                    break;
                }
            }

            if (remaining_ms.has_value() && !device_history.samples.isEmpty()) {
                const qint64 proposed_deadline_ms = device_history.samples.back().timestamp_ms + *remaining_ms;
                const std::string runtime_state_key =
                    BuildRuntimeStateKey(device, selected_component_key, use_pair_forecast);
                const auto state_it = runtime_state_key_by_device_.find(device.device_id);
                const auto deadline_it = runtime_deadline_ms_by_device_.find(device.device_id);
                if (state_it != runtime_state_key_by_device_.end() && deadline_it != runtime_deadline_ms_by_device_.end() &&
                    state_it->second == runtime_state_key) {
                    runtime_deadline_ms = ResolveStickyRuntimeDeadline(deadline_it->second, proposed_deadline_ms);
                } else {
                    runtime_deadline_ms = proposed_deadline_ms;
                }
                runtime_state_key_by_device_[device.device_id] = runtime_state_key;
                runtime_deadline_ms_by_device_[device.device_id] = runtime_deadline_ms;
                runtime_text = FormatRuntimeCountdownNoSeconds(runtime_deadline_ms - QDateTime::currentMSecsSinceEpoch());
            }
        } else {
            runtime_deadline_ms_by_device_.erase(device.device_id);
            runtime_state_key_by_device_.erase(device.device_id);
            runtime_deadline_ms_by_component_.erase(device.device_id);
            runtime_state_key_by_component_.erase(device.device_id);
        }

        auto* progress = new SmoothProgressBar(center_widget);
        progress->setObjectName(QStringLiteral("deviceProgress"));
        progress->setRange(0, 100);
        progress->setTextVisible(false);
        progress->setProperty("levelState", level_state);
        progress->setProperty("activeState", active_state);
        progress->setValue(primary.level.has_value() ? *primary.level : 0);

        const QString percent_text = primary.level.has_value()
                                         ? QStringLiteral("%1%").arg(*primary.level)
                                         : QString::fromUtf8(u8"\u041D/\u0414");
        auto* percent_chip = new SmoothPercentChip(percent_text, center_widget);
        percent_chip->setObjectName(QStringLiteral("percentChip"));
        percent_chip->setAlignment(Qt::AlignCenter);
        percent_chip->setMinimumWidth(44);
        percent_chip->setProperty("levelState", level_state);
        percent_chip->setProperty("activeState", active_state);

        QString technical_text = triplet_text;
        if (!mode_text.isEmpty()) {
            technical_text = technical_text.isEmpty() ? mode_text
                                                      : technical_text + QStringLiteral("  \u00B7  ") + mode_text;
        }
        if (!runtime_text.isEmpty()) {
            technical_text = technical_text.isEmpty() ? runtime_text
                                                      : technical_text + QStringLiteral("  \u00B7  ") + runtime_text;
        }
        if (!is_active) {
            const QString inactive_marker = QString::fromUtf8(u8"\u041D\u0435 \u043F\u043E\u0434\u043A\u043B\u044E\u0447\u0435\u043D\u043E");
            technical_text = inactive_marker;
        }

        auto* technical_label =
            new QLabel(technical_text.isEmpty() ? QStringLiteral(" ") : technical_text, center_widget);
        technical_label->setObjectName(QStringLiteral("technicalMeta"));
        technical_label->setProperty("activeState", active_state);
        technical_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        technical_label->setFixedHeight(14);
        const QString technical_prefix =
            (!triplet_text.isEmpty() && !mode_text.isEmpty())
                ? triplet_text + QStringLiteral("  ·  ") + mode_text
                : (!triplet_text.isEmpty() ? triplet_text : mode_text);
        technical_label->setProperty("runtimePrefix", technical_prefix);
        technical_label->setProperty("runtimeDeadlineMs", runtime_deadline_ms);
        technical_label->setProperty("runtimeHasDeadline", runtime_deadline_ms >= 0);
        runtime_labels_.push_back(technical_label);

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

        if (supports_noise_control) {
            auto* noise_controls = new QWidget(center_widget);
            auto* noise_layout = new QHBoxLayout(noise_controls);
            noise_layout->setContentsMargins(0, 0, 0, 0);
            noise_layout->setSpacing(4);

            auto make_mode_button = [&](const QString& text, NoiseControlMode mode, const char* mode_name) {
                auto* button = new SmoothPushButton(text, noise_controls);
                button->setProperty("smoothKind", QStringLiteral("noise"));
                button->setMinimumHeight(24);
                button->setMinimumWidth(74);
                button->setCursor(Qt::PointingHandCursor);
                const bool active_mode = NormalizeNoiseControlToken(*device.device_mode) == mode_name;
                button->setProperty("activeMode", active_mode);
                button->setStyleSheet(QStringLiteral("QPushButton { background: transparent; border: none; color: #F2F5FB; font-weight: 600; }"));
                connect(button, &QPushButton::clicked, this, [this, device_id = device.device_id, mode]() {
                    ApplyNoiseControlMode(device_id, mode);
                });
                if ((mode == NoiseControlMode::Transparency || mode == NoiseControlMode::Anc) &&
                    noise_provider->SupportsNoiseSubmodes(device.device_id, mode)) {
                    button->setContextMenuPolicy(Qt::CustomContextMenu);
                    button->setToolTip(
                        QString::fromUtf8(u8"\u041F\u0440\u0430\u0432\u044B\u0439 \u043A\u043B\u0438\u043A: "
                                          u8"\u0432\u044B\u0431\u043E\u0440 "
                                          u8"\u043F\u043E\u0434\u0440\u0435\u0436\u0438\u043C\u0430"));
                    connect(button, &QWidget::customContextMenuRequested, this,
                            [this, button, device_id = device.device_id, mode,
                             active_submode_id = device.device_submode.value_or(std::string())](const QPoint&) {
                                ShowNoiseSubmodeMenu(button, device_id, mode, active_submode_id);
                            });
                }
                noise_layout->addWidget(button);
            };

            make_mode_button(QString::fromUtf8(u8"ANC"), NoiseControlMode::Anc, "anc");
            make_mode_button(QString::fromUtf8(u8"\u041F\u0440\u043E\u0437\u0440\u0430\u0447"), NoiseControlMode::Transparency, "transparency");
            make_mode_button(QString::fromUtf8(u8"\u0412\u044B\u043A\u043B"), NoiseControlMode::Off, "off");
            center_layout->addWidget(noise_controls);
        }

        auto* actions_button = new SmoothToolButton(row);
        actions_button->setObjectName(QStringLiteral("inlineMenuButton"));
        actions_button->setProperty("smoothKind", QStringLiteral("inline"));
        actions_button->setText(QString::fromUtf8(u8"\u22EF"));
        actions_button->setPopupMode(QToolButton::InstantPopup);
        actions_button->setToolTip(QString::fromUtf8(u8"\u0414\u0435\u0439\u0441\u0442\u0432\u0438\u044F"));

        auto* actions_menu = new QMenu(actions_button);
        auto* refresh_row_action = actions_menu->addAction(QString::fromUtf8(u8"\u041E\u0431\u043D\u043E\u0432\u0438\u0442\u044C"));
        auto* diagnostics_row_action =
            actions_menu->addAction(QString::fromUtf8(u8"\u0414\u0438\u0430\u0433\u043D\u043E\u0441\u0442\u0438\u043A\u0430"));
        auto* stats_row_action =
            actions_menu->addAction(QString::fromUtf8(u8"\u0421\u0442\u0430\u0442\u0438\u0441\u0442\u0438\u043A\u0430"));
        auto* history_row_action =
            actions_menu->addAction(QString::fromUtf8(u8"\u0413\u0440\u0430\u0444\u0438\u043A "
                                                      u8"\u0440\u0430\u0437\u0440\u044F\u0434\u043A\u0438"));
        auto* hide_row_action =
            actions_menu->addAction(QString::fromUtf8(u8"\u0421\u043A\u0440\u044B\u0442\u044C "
                                                      u8"\u0443\u0441\u0442\u0440\u043E\u0439\u0441\u0442\u0432\u043E"));
        actions_button->setMenu(actions_menu);

        connect(refresh_row_action, &QAction::triggered, this, [this, device_id = device.device_id]() {
            if (bluetooth_refresh_debounce_timer_ != nullptr) {
                bluetooth_refresh_debounce_timer_->stop();
            }
            pending_bluetooth_refresh_device_id_.clear();
            RefreshBatteryDataForDevice(device_id, true);
        });
        connect(diagnostics_row_action, &QAction::triggered, this,
                [this, device_id = device.device_id]() { ShowDeviceDiagnostics(device_id); });
        connect(stats_row_action, &QAction::triggered, this,
                [this, device_id = device.device_id, device_name = device.device_name]() {
                    ShowBatteryStats(device_id, device_name);
                });
        connect(history_row_action, &QAction::triggered, this,
                [this, device_id = device.device_id, device_name = device.device_name]() {
                    ShowBatteryHistory(device_id, device_name);
                });
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

void BatteryWindow::UpdateRuntimeCountdownLabels() {
    if (runtime_labels_.empty()) {
        return;
    }

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    auto it = runtime_labels_.begin();
    while (it != runtime_labels_.end()) {
        if (it->isNull()) {
            it = runtime_labels_.erase(it);
            continue;
        }

        QLabel* label = it->data();
        if (label == nullptr || !label->property("runtimeHasDeadline").toBool()) {
            ++it;
            continue;
        }

        const qint64 deadline_ms = label->property("runtimeDeadlineMs").toLongLong();
        const QString runtime_prefix = label->property("runtimePrefix").toString();
        const QString runtime_text = FormatRuntimeCountdownNoSeconds(deadline_ms - now_ms);
        label->setText(runtime_prefix.isEmpty() ? runtime_text
                                                : runtime_prefix + QStringLiteral("  ·  ") + runtime_text);
        ++it;
    }
}

void BatteryWindow::ClearDeviceCards() {
    if (cards_layout_ == nullptr) {
        return;
    }

    runtime_labels_.clear();

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
