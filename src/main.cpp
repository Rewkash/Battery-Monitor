#include <chrono>
#include <exception>
#include <iostream>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef BATTERY_MONITOR_WITH_QT
#include <QApplication>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include "ui/BatteryWindow.h"
#ifdef BATTERY_MONITOR_WITH_UPDATER
#include "update/UpdateService.h"
#endif
#endif

#include "AppMain.h"
#include "app/BatteryOutputFormatter.h"
#include "app/CommandLineOptions.h"
#include "app/PlatformCommandDispatcher.h"
#include "core/ProviderFactory.h"
#include "BatteryMonitorVersion.h"
#ifdef BATTERY_MONITOR_WITH_UPDATER
#include "update/StartupHealth.h"
#endif

namespace battery_monitor {

namespace {

#ifdef _WIN32
bool ClaimGuiInstance() {
    static HANDLE instance_mutex = CreateMutexW(nullptr, TRUE, L"Local\\ChargeViewGuiInstance");
    if (instance_mutex == nullptr) {
        return true;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instance_mutex);
        instance_mutex = nullptr;
        return false;
    }

    return true;
}
#endif

int RunCliApplication([[maybe_unused]] int argc, [[maybe_unused]] char** argv, const CommandLineOptions& options) {
    auto provider = CreateBatteryProvider();
#ifdef BATTERY_MONITOR_WITH_UPDATER
    SignalUpdateStartupHealth(argc, argv);
#endif
    BatteryQueryOptions query_options;
    query_options.operation.deadline =
        ProviderOperationContext::Clock::now() + std::chrono::seconds(15);
    query_options.include_disconnected = options.include_offline;
    const auto devices = provider->GetDevicesBattery(query_options);
    const auto format = options.json_output ? DeviceListOutputFormat::Json : DeviceListOutputFormat::Table;
    PrintDevices(devices, format, std::cout);
    return 0;
}

int RunGuiApplication(int argc, char** argv) {
#ifdef BATTERY_MONITOR_WITH_QT
    QApplication app(argc, argv);
    QApplication::setApplicationDisplayName(QStringLiteral("ChargeView"));
#ifdef _WIN32
    if (!ClaimGuiInstance()) {
        return 0;
    }
#endif
    QCoreApplication::setOrganizationName(QStringLiteral(BATTERY_MONITOR_PUBLISHER));
    QCoreApplication::setApplicationName(QStringLiteral("Battery Monitor"));
    QCoreApplication::setApplicationVersion(QStringLiteral(BATTERY_MONITOR_VERSION));
    auto provider = CreateBatteryProvider();
    BatteryWindow window(std::move(provider));
    window.Launch();
#ifdef BATTERY_MONITOR_WITH_UPDATER
    QTimer::singleShot(0, &app, [argc, argv]() { SignalUpdateStartupHealth(argc, argv); });
#endif
    return app.exec();
#else
    (void)argc;
    (void)argv;
    return 0;
#endif
}

#ifdef BATTERY_MONITOR_WITH_QT
int RunUpdateCheck(int argc, char** argv, bool json_output) {
#ifdef BATTERY_MONITOR_WITH_UPDATER
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral(BATTERY_MONITOR_PUBLISHER));
    QCoreApplication::setApplicationName(QStringLiteral("Battery Monitor"));
    QCoreApplication::setApplicationVersion(QStringLiteral(BATTERY_MONITOR_VERSION));
    UpdateService service;
    int result = 20;
    QObject::connect(&service, &UpdateService::CheckFinished, &app,
                     [&](bool available, const UpdateManifest& manifest, const QString& error) {
        if (!error.isEmpty()) {
            if (json_output) {
                const QJsonObject output{{QStringLiteral("schemaVersion"), 1},
                                         {QStringLiteral("error"), error}};
                std::cout << QJsonDocument(output).toJson(QJsonDocument::Compact).constData() << '\n';
            } else {
                std::cerr << "Update check failed: " << error.toStdString() << '\n';
            }
            result = 20;
        } else if (available) {
            if (json_output) {
                std::cout << "{\"schemaVersion\":1,\"updateAvailable\":true,\"version\":\""
                          << manifest.version.toStdString() << "\",\"mandatory\":"
                          << (manifest.mandatory ? "true" : "false") << "}\n";
            } else {
                std::cout << "Update available: " << manifest.version.toStdString() << '\n';
            }
            result = manifest.mandatory ? 11 : 10;
        } else {
            std::cout << (json_output ? "{\"schemaVersion\":1,\"updateAvailable\":false}\n"
                                      : "No update available.\n");
            result = 0;
        }
        app.quit();
    });
    QTimer::singleShot(0, &service, [&service]() { service.CheckForUpdates(true); });
    app.exec();
    return result;
#else
    (void)argc;
    (void)argv;
    (void)json_output;
    std::cerr << "Updater is not available in this build.\n";
    return 20;
#endif
}
#endif

}  // namespace

}  // namespace battery_monitor

int battery_monitor::BatteryMonitorMain(int argc, char** argv, [[maybe_unused]] bool prefer_gui) {
    const CommandLineOptions options = ParseCommandLine(argc, argv);

    if (options.has_error) {
        std::cerr << "Error: " << options.error_message << "\n\n" << CommandLineUsageText();
        return 2;
    }

    try {
        if (options.show_version) {
            std::cout << BATTERY_MONITOR_VERSION << '\n';
            return 0;
        }
#ifdef BATTERY_MONITOR_WITH_QT
        if (options.check_updates) {
            return RunUpdateCheck(argc, argv, options.json_output);
        }
#endif
        if (const auto exit_code = TryRunPlatformCommand(options.platform_commands); exit_code.has_value()) {
            return *exit_code;
        }
#ifdef BATTERY_MONITOR_WITH_QT
        if (ShouldLaunchGui(options, prefer_gui)) {
            return RunGuiApplication(argc, argv);
        }
#endif
        return RunCliApplication(argc, argv, options);
    } catch (const std::exception& ex) {
        if (!PrintPlatformException(ex, std::cerr)) {
            std::cerr << "Error: " << ex.what() << '\n';
        }
        return 1;
    } catch (...) {
        std::cerr << "Error: unknown exception\n";
        return 1;
    }
}
