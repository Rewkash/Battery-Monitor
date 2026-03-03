#include "ui/BatteryWindow.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
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
};

struct DeviceEntry {
    std::string device_id;
    std::string device_name;
    std::vector<ComponentEntry> components;
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

QString ComponentDisplayName(const std::string& component) {
    if (component == "left") {
        return QStringLiteral("Left Earbud");
    }
    if (component == "right") {
        return QStringLiteral("Right Earbud");
    }
    if (component == "case") {
        return QStringLiteral("Charging Case");
    }
    if (component == "main") {
        return QStringLiteral("Main Battery");
    }

    if (component.empty()) {
        return QStringLiteral("Component");
    }

    std::string title = component;
    title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
    return ToQString(title);
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

std::vector<DeviceEntry> GroupDevices(const std::vector<DeviceBatteryInfo>& devices) {
    std::vector<DeviceEntry> grouped;
    std::unordered_map<std::string, std::size_t> device_index;

    for (const auto& item : devices) {
        const std::string device_id = item.device_id.empty() ? "UnknownDevice" : item.device_id;
        const std::string device_name = item.device_name.empty() ? "Unknown device" : item.device_name;

        std::size_t index = 0;
        const auto found = device_index.find(device_id);
        if (found == device_index.end()) {
            index = grouped.size();
            device_index.emplace(device_id, index);

            DeviceEntry entry;
            entry.device_id = device_id;
            entry.device_name = device_name;
            grouped.push_back(std::move(entry));
        } else {
            index = found->second;
            if (grouped[index].device_name == "Unknown device" && device_name != "Unknown device") {
                grouped[index].device_name = device_name;
            }
        }

        auto& device_entry = grouped[index];
        ComponentEntry incoming;
        incoming.component = NormalizeComponentName(item.battery_component);
        incoming.battery_level_percent = item.battery_level_percent;
        incoming.is_cached = item.is_cached;

        auto existing = std::find_if(device_entry.components.begin(), device_entry.components.end(),
                                     [&incoming](const ComponentEntry& current) {
                                         return current.component == incoming.component;
                                     });

        if (existing == device_entry.components.end()) {
            device_entry.components.push_back(std::move(incoming));
            continue;
        }

        if (ComponentQualityScore(incoming) > ComponentQualityScore(*existing)) {
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
        const auto lhs_name = ToLowerAscii(lhs.device_name);
        const auto rhs_name = ToLowerAscii(rhs.device_name);
        if (lhs_name != rhs_name) {
            return lhs_name < rhs_name;
        }
        return lhs.device_id < rhs.device_id;
    });

    return grouped;
}

QString SourceState(const ComponentEntry& component) {
    if (!component.battery_level_percent.has_value()) {
        return QStringLiteral("na");
    }
    return component.is_cached ? QStringLiteral("cached") : QStringLiteral("live");
}

QString SourceLabel(const QString& source_state) {
    if (source_state == QStringLiteral("live")) {
        return QStringLiteral("LIVE");
    }
    if (source_state == QStringLiteral("cached")) {
        return QStringLiteral("CACHED");
    }
    return QStringLiteral("N/A");
}

QString DeviceState(const DeviceEntry& device) {
    const auto has_live = std::any_of(device.components.begin(), device.components.end(), [](const ComponentEntry& item) {
        return item.battery_level_percent.has_value() && !item.is_cached;
    });
    if (has_live) {
        return QStringLiteral("live");
    }

    const auto has_cached = std::any_of(device.components.begin(), device.components.end(), [](const ComponentEntry& item) {
        return item.battery_level_percent.has_value() && item.is_cached;
    });
    if (has_cached) {
        return QStringLiteral("cached");
    }

    return QStringLiteral("na");
}

QString DeviceStateLabel(const QString& state) {
    if (state == QStringLiteral("live")) {
        return QStringLiteral("LIVE DATA");
    }
    if (state == QStringLiteral("cached")) {
        return QStringLiteral("CACHED DATA");
    }
    return QStringLiteral("NO BATTERY DATA");
}

QWidget* CreateComponentRow(const ComponentEntry& component, QWidget* parent) {
    auto* row = new QWidget(parent);
    row->setObjectName(QStringLiteral("componentRow"));

    auto* row_layout = new QGridLayout(row);
    row_layout->setContentsMargins(12, 10, 12, 10);
    row_layout->setHorizontalSpacing(12);
    row_layout->setVerticalSpacing(4);

    auto* component_label = new QLabel(ComponentDisplayName(component.component), row);
    component_label->setObjectName(QStringLiteral("componentLabel"));

    auto* progress_bar = new QProgressBar(row);
    progress_bar->setObjectName(QStringLiteral("batteryBar"));
    progress_bar->setRange(0, 100);
    progress_bar->setTextVisible(true);
    progress_bar->setMinimumWidth(260);
    progress_bar->setMinimumHeight(20);

    const QString source_state = SourceState(component);
    progress_bar->setProperty("state", source_state);

    QString value_text = QStringLiteral("N/A");
    if (component.battery_level_percent.has_value()) {
        const auto battery_value = static_cast<int>(*component.battery_level_percent);
        progress_bar->setValue(battery_value);
        progress_bar->setFormat(QStringLiteral("%1%").arg(battery_value));
        value_text = QStringLiteral("%1%").arg(battery_value);
    } else {
        progress_bar->setValue(0);
        progress_bar->setFormat(QStringLiteral("N/A"));
    }

    auto* value_label = new QLabel(value_text, row);
    value_label->setObjectName(QStringLiteral("valueLabel"));
    value_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto* source_label = new QLabel(SourceLabel(source_state), row);
    source_label->setObjectName(QStringLiteral("sourceBadge"));
    source_label->setProperty("state", source_state);
    source_label->setAlignment(Qt::AlignCenter);

    row_layout->addWidget(component_label, 0, 0);
    row_layout->addWidget(progress_bar, 0, 1);
    row_layout->addWidget(value_label, 0, 2);
    row_layout->addWidget(source_label, 0, 3);
    row_layout->setColumnStretch(1, 1);

    return row;
}

QWidget* CreateDeviceCard(const DeviceEntry& device, QWidget* parent) {
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("deviceCard"));

    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(16, 14, 16, 14);
    card_layout->setSpacing(10);

    auto* header_layout = new QHBoxLayout();
    header_layout->setSpacing(10);

    auto* title_container = new QWidget(card);
    auto* title_layout = new QVBoxLayout(title_container);
    title_layout->setContentsMargins(0, 0, 0, 0);
    title_layout->setSpacing(2);

    auto* title_label = new QLabel(ToQString(device.device_name), title_container);
    title_label->setObjectName(QStringLiteral("deviceTitle"));

    auto* id_label = new QLabel(ToQString(device.device_id), title_container);
    id_label->setObjectName(QStringLiteral("deviceId"));
    id_label->setTextInteractionFlags(Qt::TextSelectableByMouse);

    title_layout->addWidget(title_label);
    title_layout->addWidget(id_label);

    const QString device_state = DeviceState(device);
    auto* state_badge = new QLabel(DeviceStateLabel(device_state), card);
    state_badge->setObjectName(QStringLiteral("deviceStateBadge"));
    state_badge->setProperty("state", device_state);
    state_badge->setAlignment(Qt::AlignCenter);

    header_layout->addWidget(title_container, 1);
    header_layout->addWidget(state_badge, 0, Qt::AlignTop);
    card_layout->addLayout(header_layout);

    if (device.components.empty()) {
        auto* empty_label = new QLabel(QStringLiteral("No battery components for this device."), card);
        empty_label->setObjectName(QStringLiteral("emptyState"));
        card_layout->addWidget(empty_label);
        return card;
    }

    for (const auto& component : device.components) {
        card_layout->addWidget(CreateComponentRow(component, card));
    }

    return card;
}

QString MakeSummaryText(const std::vector<DeviceEntry>& grouped_devices) {
    int components = 0;
    int live_count = 0;
    int cached_count = 0;
    int no_data_count = 0;

    for (const auto& device : grouped_devices) {
        for (const auto& component : device.components) {
            ++components;
            if (!component.battery_level_percent.has_value()) {
                ++no_data_count;
                continue;
            }
            if (component.is_cached) {
                ++cached_count;
            } else {
                ++live_count;
            }
        }
    }

    return QStringLiteral("Devices: %1   Components: %2   Live: %3   Cached: %4   N/A: %5")
        .arg(grouped_devices.size())
        .arg(components)
        .arg(live_count)
        .arg(cached_count)
        .arg(no_data_count);
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
    setObjectName(QStringLiteral("batteryWindow"));
    setWindowTitle(QStringLiteral("Battery Monitor"));
    resize(1100, 700);
    setMinimumSize(920, 560);

    setStyleSheet(R"(
QWidget#batteryWindow {
    background: #F4F7FB;
    color: #0F172A;
}
QLabel#appTitle {
    font-size: 24px;
    font-weight: 700;
    color: #0F172A;
}
QLabel#appSubtitle {
    font-size: 12px;
    color: #64748B;
}
QLabel#summaryLabel {
    font-size: 12px;
    color: #334155;
    font-weight: 600;
}
QLabel#statusLabel {
    font-size: 12px;
    color: #475569;
}
QPushButton#refreshButton {
    background: #0EA5E9;
    color: #FFFFFF;
    font-weight: 700;
    border: 0;
    border-radius: 10px;
    padding: 8px 18px;
}
QPushButton#refreshButton:hover {
    background: #0284C7;
}
QPushButton#refreshButton:disabled {
    background: #94A3B8;
}
QFrame#deviceCard {
    background: #FFFFFF;
    border: 1px solid #D6E1EE;
    border-radius: 14px;
}
QLabel#deviceTitle {
    font-size: 18px;
    font-weight: 700;
    color: #0F172A;
}
QLabel#deviceId {
    font-size: 11px;
    color: #64748B;
}
QLabel#deviceStateBadge {
    border-radius: 999px;
    padding: 4px 10px;
    font-size: 11px;
    font-weight: 700;
}
QLabel#deviceStateBadge[state="live"] {
    background: #DCFCE7;
    color: #166534;
    border: 1px solid #86EFAC;
}
QLabel#deviceStateBadge[state="cached"] {
    background: #FEF3C7;
    color: #92400E;
    border: 1px solid #FCD34D;
}
QLabel#deviceStateBadge[state="na"] {
    background: #E2E8F0;
    color: #334155;
    border: 1px solid #CBD5E1;
}
QWidget#componentRow {
    background: #F8FAFC;
    border: 1px solid #E2E8F0;
    border-radius: 10px;
}
QLabel#componentLabel {
    font-size: 12px;
    font-weight: 600;
    color: #0F172A;
}
QProgressBar#batteryBar {
    border: 1px solid #CBD5E1;
    border-radius: 6px;
    background: #FFFFFF;
    color: #0F172A;
    text-align: center;
    font-weight: 700;
}
QProgressBar#batteryBar[state="live"]::chunk {
    background: #38BDF8;
    border-radius: 5px;
}
QProgressBar#batteryBar[state="cached"]::chunk {
    background: #F59E0B;
    border-radius: 5px;
}
QProgressBar#batteryBar[state="na"]::chunk {
    background: #CBD5E1;
    border-radius: 5px;
}
QLabel#valueLabel {
    font-size: 12px;
    font-weight: 700;
    color: #0F172A;
    min-width: 48px;
}
QLabel#sourceBadge {
    border-radius: 999px;
    padding: 3px 8px;
    font-size: 10px;
    font-weight: 700;
}
QLabel#sourceBadge[state="live"] {
    background: #DBEAFE;
    color: #1D4ED8;
    border: 1px solid #93C5FD;
}
QLabel#sourceBadge[state="cached"] {
    background: #FEF3C7;
    color: #92400E;
    border: 1px solid #FCD34D;
}
QLabel#sourceBadge[state="na"] {
    background: #E2E8F0;
    color: #334155;
    border: 1px solid #CBD5E1;
}
QLabel#emptyState {
    color: #475569;
    font-size: 13px;
}
)");

    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(18, 16, 18, 16);
    root_layout->setSpacing(10);

    auto* header_layout = new QHBoxLayout();
    header_layout->setSpacing(10);

    auto* title_layout = new QVBoxLayout();
    title_layout->setSpacing(0);

    auto* title_label = new QLabel(QStringLiteral("Battery Monitor"), this);
    title_label->setObjectName(QStringLiteral("appTitle"));

    auto* subtitle_label = new QLabel(QStringLiteral("Bluetooth devices grouped by component battery"), this);
    subtitle_label->setObjectName(QStringLiteral("appSubtitle"));

    title_layout->addWidget(title_label);
    title_layout->addWidget(subtitle_label);

    refresh_button_ = new QPushButton(QStringLiteral("Refresh"), this);
    refresh_button_->setObjectName(QStringLiteral("refreshButton"));

    header_layout->addLayout(title_layout, 1);
    header_layout->addWidget(refresh_button_, 0, Qt::AlignTop);

    auto* info_layout = new QHBoxLayout();
    info_layout->setSpacing(8);

    summary_label_ = new QLabel(QStringLiteral("Devices: 0   Components: 0   Live: 0   Cached: 0   N/A: 0"), this);
    summary_label_->setObjectName(QStringLiteral("summaryLabel"));

    status_label_ = new QLabel(QStringLiteral("Ready"), this);
    status_label_->setObjectName(QStringLiteral("statusLabel"));
    status_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    status_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    info_layout->addWidget(summary_label_, 1);
    info_layout->addWidget(status_label_);

    auto* scroll_area = new QScrollArea(this);
    scroll_area->setWidgetResizable(true);
    scroll_area->setFrameShape(QFrame::NoFrame);

    cards_container_ = new QWidget(scroll_area);
    cards_layout_ = new QVBoxLayout(cards_container_);
    cards_layout_->setContentsMargins(2, 2, 2, 2);
    cards_layout_->setSpacing(12);

    scroll_area->setWidget(cards_container_);

    root_layout->addLayout(header_layout);
    root_layout->addLayout(info_layout);
    root_layout->addWidget(scroll_area, 1);

    connect(refresh_button_, &QPushButton::clicked, this, [this]() { RefreshBatteryData(); });

    RefreshBatteryData();
}

void BatteryWindow::RefreshBatteryData() {
    refresh_button_->setEnabled(false);
    status_label_->setText(QStringLiteral("Refreshing..."));

    try {
        const auto devices = provider_->GetConnectedDevicesBattery();
        PopulateDeviceCards(devices);

        if (devices.empty()) {
            status_label_->setText(QStringLiteral("No connected Bluetooth devices with battery data."));
        } else {
            status_label_->setText(QStringLiteral("Updated."));
        }
#ifdef _WIN32
    } catch (const winrt::hresult_error& error) {
        ClearDeviceCards();
        summary_label_->setText(QStringLiteral("Devices: 0   Components: 0   Live: 0   Cached: 0   N/A: 0"));

        auto* error_label = new QLabel(QStringLiteral("Unable to query Bluetooth stack."), cards_container_);
        error_label->setObjectName(QStringLiteral("emptyState"));
        error_label->setAlignment(Qt::AlignCenter);
        cards_layout_->addWidget(error_label);
        cards_layout_->addStretch(1);

        status_label_->setText(QStringLiteral("Error: %1").arg(FormatWinRtError(error)));
#endif
    } catch (const std::exception& ex) {
        ClearDeviceCards();
        summary_label_->setText(QStringLiteral("Devices: 0   Components: 0   Live: 0   Cached: 0   N/A: 0"));

        auto* error_label = new QLabel(QStringLiteral("Unable to query battery data."), cards_container_);
        error_label->setObjectName(QStringLiteral("emptyState"));
        error_label->setAlignment(Qt::AlignCenter);
        cards_layout_->addWidget(error_label);
        cards_layout_->addStretch(1);

        status_label_->setText(QStringLiteral("Error: %1").arg(ToQString(FormatError(ex))));
    } catch (...) {
        ClearDeviceCards();
        summary_label_->setText(QStringLiteral("Devices: 0   Components: 0   Live: 0   Cached: 0   N/A: 0"));

        auto* error_label = new QLabel(QStringLiteral("Unable to query battery data."), cards_container_);
        error_label->setObjectName(QStringLiteral("emptyState"));
        error_label->setAlignment(Qt::AlignCenter);
        cards_layout_->addWidget(error_label);
        cards_layout_->addStretch(1);

        status_label_->setText(QStringLiteral("Error: unknown exception"));
    }

    refresh_button_->setEnabled(true);
}

void BatteryWindow::PopulateDeviceCards(const std::vector<DeviceBatteryInfo>& devices) {
    ClearDeviceCards();

    const auto grouped_devices = GroupDevices(devices);
    summary_label_->setText(MakeSummaryText(grouped_devices));

    if (grouped_devices.empty()) {
        auto* empty_label = new QLabel(QStringLiteral("No Bluetooth device with battery info was found."), cards_container_);
        empty_label->setObjectName(QStringLiteral("emptyState"));
        empty_label->setAlignment(Qt::AlignCenter);
        cards_layout_->addWidget(empty_label);
        cards_layout_->addStretch(1);
        return;
    }

    for (const auto& device : grouped_devices) {
        cards_layout_->addWidget(CreateDeviceCard(device, cards_container_));
    }

    cards_layout_->addStretch(1);
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
