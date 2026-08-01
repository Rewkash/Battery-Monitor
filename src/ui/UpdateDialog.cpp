#include "ui/UpdateDialog.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QLabel>
#include <limits>
#include <QProgressBar>
#include <QPushButton>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace battery_monitor {

UpdateDialog::UpdateDialog(const UpdateManifest& manifest, QWidget* parent)
    : QDialog(parent), manifest_(manifest) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QString::fromUtf8(u8"Обновление ChargeView"));
    setMinimumWidth(470);

    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel(
        QString::fromUtf8(u8"Доступна версия %1%2")
            .arg(manifest.version, manifest.mandatory ? QString::fromUtf8(u8" — обязательное обновление") : QString()),
        this);
    title->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 700;"));
    layout->addWidget(title);

    auto* notes = new QTextBrowser(this);
    notes->setOpenExternalLinks(true);
    notes->setMarkdown(manifest.release_notes.isEmpty()
                           ? QString::fromUtf8(u8"Примечания к выпуску не указаны.")
                           : manifest.release_notes);
    notes->setMaximumHeight(220);
    layout->addWidget(notes);

    if (manifest.release_notes_url.isValid()) {
        auto* release_link = new QLabel(
            QString::fromUtf8(u8"<a href=\"%1\">Открыть страницу выпуска на GitHub</a>")
                .arg(manifest.release_notes_url.toString().toHtmlEscaped()),
            this);
        release_link->setOpenExternalLinks(true);
        layout->addWidget(release_link);
    }

    status_label_ = new QLabel(QString::fromUtf8(u8"Пакет будет проверен по Ed25519 и SHA-256."), this);
    status_label_->setWordWrap(true);
    layout->addWidget(status_label_);

    progress_ = new QProgressBar(this);
    progress_->setVisible(false);
    layout->addWidget(progress_);

    auto* buttons = new QDialogButtonBox(this);
    install_button_ = buttons->addButton(QString::fromUtf8(u8"Скачать и перезапустить"), QDialogButtonBox::AcceptRole);
    later_button_ = buttons->addButton(QString::fromUtf8(u8"Позже"), QDialogButtonBox::RejectRole);
    later_button_->setVisible(!manifest.mandatory);
    connect(install_button_, &QPushButton::clicked, this, [this]() { emit InstallRequested(); });
    connect(later_button_, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void UpdateDialog::SetDownloadProgress(qint64 received, qint64 total) {
    progress_->setVisible(true);
    progress_->setRange(0, total > 0 && total <= std::numeric_limits<int>::max() ? static_cast<int>(total) : 0);
    progress_->setValue(progress_->maximum() > 0 ? static_cast<int>(received) : 0);
    install_button_->setEnabled(false);
    status_label_->setText(QString::fromUtf8(u8"Загрузка и проверка обновления…"));
}

void UpdateDialog::SetInstalling() {
    status_label_->setText(QString::fromUtf8(u8"Обновление проверено. Приложение перезапускается…"));
    progress_->setRange(0, 0);
    install_button_->setEnabled(false);
}

void UpdateDialog::SetError(const QString& error) {
    status_label_->setText(QString::fromUtf8(u8"Обновление не установлено: %1").arg(error));
    progress_->setVisible(false);
    install_button_->setEnabled(true);
}

}  // namespace battery_monitor
