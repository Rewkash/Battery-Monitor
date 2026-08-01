#include "ui/DeviceDiagnosticsDialog.h"

#include <algorithm>
#include <sstream>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QTextStream>
#include <QVBoxLayout>

namespace battery_monitor {

namespace {

QString ToQString(const std::string& value) {
    return QString::fromUtf8(value.c_str());
}

QString OptionalPercentText(const std::optional<std::uint8_t>& value) {
    return value.has_value() ? QStringLiteral("%1%").arg(*value) : QStringLiteral("not available");
}

QString OptionalStringText(const std::optional<std::string>& value) {
    return value.has_value() ? ToQString(*value) : QStringLiteral("not available");
}

QString OptionalUIntText(const std::optional<std::uint32_t>& value) {
    return value.has_value() ? QString::number(*value) : QStringLiteral("not available");
}

QString OptionalUShortText(const std::optional<std::uint16_t>& value) {
    return value.has_value() ? QString::number(*value) : QStringLiteral("not available");
}

QString ResolveDiagnosticsDirectory() {
    QString base_path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base_path.trimmed().isEmpty()) {
        base_path = QDir::home().filePath(QStringLiteral(".battery-monitor"));
    }

    QDir directory(base_path);
    directory.mkpath(QStringLiteral("diagnostics"));
    return directory.filePath(QStringLiteral("diagnostics"));
}

QString SafeFileToken(QString value) {
    value = value.trimmed();
    if (value.isEmpty()) {
        value = QStringLiteral("device");
    }
    for (QChar& ch : value) {
        if (!ch.isLetterOrNumber()) {
            ch = QLatin1Char('_');
        }
    }
    return value.left(80);
}

}  // namespace

DeviceDiagnosticsDialog::DeviceDiagnosticsDialog(std::vector<DeviceBatteryInfo> entries, QWidget* parent)
    : QDialog(parent), entries_(std::move(entries)) {
    setWindowTitle(QString::fromUtf8(u8"Диагностика устройства"));
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(760, 620);
    BuildUi();
}

void DeviceDiagnosticsDialog::BuildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(8);

    title_label_ = new QLabel(this);
    title_label_->setText(entries_.empty() ? QString::fromUtf8(u8"Устройство") : ToQString(entries_.front().device_name));
    title_label_->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 700;"));

    const QString diagnostics_text = BuildDiagnosticsText();
    log_file_path_ = WriteDiagnosticsLog(diagnostics_text);

    log_path_label_ = new QLabel(this);
    log_path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    log_path_label_->setText(log_file_path_.isEmpty()
                                 ? QString::fromUtf8(u8"Лог диагностики не записан")
                                 : QString::fromUtf8(u8"Лог: %1").arg(log_file_path_));

    diagnostics_view_ = new QTextBrowser(this);
    diagnostics_view_->setPlainText(diagnostics_text);
    diagnostics_view_->setLineWrapMode(QTextEdit::NoWrap);

    auto* close_button = new QPushButton(QString::fromUtf8(u8"Закрыть"), this);
    connect(close_button, &QPushButton::clicked, this, &QDialog::accept);

    layout->addWidget(title_label_);
    layout->addWidget(log_path_label_);
    layout->addWidget(diagnostics_view_, 1);
    layout->addWidget(close_button, 0, Qt::AlignRight);
}

QString DeviceDiagnosticsDialog::BuildDiagnosticsText() const {
    QString output;
    QTextStream stream(&output);
    const auto now = QDateTime::currentDateTime();

    stream << "ChargeView device diagnostics\n";
    stream << "Generated: " << now.toString(Qt::ISODate) << "\n";
    stream << "Entry count: " << entries_.size() << "\n\n";

    if (entries_.empty()) {
        stream << "No entries for this device.\n";
        return output;
    }

    const auto& first = entries_.front();
    stream << "Device\n";
    stream << "  name: " << ToQString(first.device_name) << "\n";
    stream << "  id: " << ToQString(first.device_id) << "\n";
    stream << "  connected: " << (first.is_connected ? "true" : "false") << "\n";
    stream << "  mode: " << OptionalStringText(first.device_mode) << "\n";
    stream << "  submode: " << OptionalStringText(first.device_submode) << "\n";
    stream << "  bluetooth LE appearance: " << OptionalUShortText(first.bluetooth_le_appearance) << "\n";
    stream << "  bluetooth COD major: " << OptionalUIntText(first.bluetooth_cod_major) << "\n";
    stream << "  bluetooth COD minor: " << OptionalUIntText(first.bluetooth_cod_minor) << "\n";
    stream << "  categories: ";
    if (first.device_categories.empty()) {
        stream << "none";
    } else {
        for (std::size_t i = 0; i < first.device_categories.size(); ++i) {
            if (i > 0) {
                stream << ", ";
            }
            stream << ToQString(first.device_categories[i]);
        }
    }
    stream << "\n\n";

    stream << "Battery entries\n";
    for (const auto& entry : entries_) {
        stream << "  component: " << ToQString(entry.battery_component.empty() ? "main" : entry.battery_component) << "\n";
        stream << "    percent: " << OptionalPercentText(entry.battery_level_percent) << "\n";
        stream << "    cached: " << (entry.is_cached ? "true" : "false") << "\n";
        stream << "    connected: " << (entry.is_connected ? "true" : "false") << "\n";
    }

    stream << "\nInterpretation\n";
    const bool has_live_battery = std::any_of(entries_.begin(), entries_.end(), [](const DeviceBatteryInfo& entry) {
        return entry.is_connected && !entry.is_cached && entry.battery_level_percent.has_value();
    });
    const bool has_cached_battery = std::any_of(entries_.begin(), entries_.end(), [](const DeviceBatteryInfo& entry) {
        return entry.battery_level_percent.has_value() && entry.is_cached;
    });
    if (has_live_battery) {
        stream << "  Battery data source: live provider result\n";
    } else if (has_cached_battery) {
        stream << "  Battery data source: cached/fallback provider result\n";
    } else if (!first.is_connected) {
        stream << "  Battery data source: unavailable because device is disconnected\n";
    } else {
        stream << "  Battery data source: no battery value exposed by current readers\n";
    }

    return output;
}

QString DeviceDiagnosticsDialog::WriteDiagnosticsLog(const QString& diagnostics_text) const {
    const QString directory_path = ResolveDiagnosticsDirectory();
    QDir directory(directory_path);
    const QString device_name = entries_.empty() ? QStringLiteral("device") : ToQString(entries_.front().device_name);
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString file_path = directory.filePath(
        QStringLiteral("%1-%2.log").arg(timestamp, SafeFileToken(device_name)));

    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        return {};
    }

    QTextStream stream(&file);
    stream << diagnostics_text;
    return file_path;
}

}  // namespace battery_monitor
