#include "update/WindowsInstallMode.h"

#include "BatteryMonitorInstallerIdentity.h"

#include <windows.h>

#include <array>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace battery_monitor {
namespace {

constexpr wchar_t kInstallerKey[] = L"Software\\Orion Group\\Battery Monitor\\Install";

bool ReadRegistryString(const wchar_t* name, QString* value) {
    std::array<wchar_t, 32768> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, kInstallerKey, name, RRF_RT_REG_SZ, nullptr,
                     buffer.data(), &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) {
        return false;
    }
    const qsizetype character_count = static_cast<qsizetype>(size / sizeof(wchar_t));
    const qsizetype text_length = character_count > 0 && buffer[character_count - 1] == L'\0'
                                      ? character_count - 1
                                      : character_count;
    *value = QString::fromWCharArray(buffer.data(), text_length);
    return true;
}

QString CanonicalPath(const QString& path) {
    const QString canonical = QDir(path).canonicalPath();
    return QDir::cleanPath(canonical.isEmpty() ? QDir(path).absolutePath() : canonical);
}

}  // namespace

WindowsInstallMode DetectWindowsInstallMode() {
    QString mode;
    QString install_location;
    QString upgrade_code;
    if (!ReadRegistryString(L"InstallMode", &mode) ||
        !ReadRegistryString(L"InstallLocation", &install_location) ||
        !ReadRegistryString(L"UpgradeCode", &upgrade_code) ||
        mode != QStringLiteral("msi-per-user") ||
        upgrade_code.compare(QStringLiteral(BATTERY_MONITOR_MSI_UPGRADE_CODE), Qt::CaseInsensitive) != 0 ||
        CanonicalPath(install_location).compare(CanonicalPath(QCoreApplication::applicationDirPath()),
                                                Qt::CaseInsensitive) != 0) {
        return WindowsInstallMode::Portable;
    }
    return WindowsInstallMode::PerUserMsi;
}

}  // namespace battery_monitor
