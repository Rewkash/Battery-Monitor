#include <exception>
#include <iostream>
#include <memory>

#ifdef BATTERY_MONITOR_WITH_QT
#include <QApplication>

#include "ui/BatteryWindow.h"
#endif

#include "AppMain.h"
#include "app/BatteryOutputFormatter.h"
#include "app/CommandLineOptions.h"
#include "app/PlatformCommandDispatcher.h"
#include "core/ProviderFactory.h"

namespace battery_monitor {

namespace {

int RunCliApplication(const CommandLineOptions& options) {
    auto provider = CreateBatteryProvider();
    BatteryQueryOptions query_options;
    query_options.include_disconnected = options.include_offline;
    const auto devices = provider->GetDevicesBattery(query_options);
    const auto format = options.json_output ? DeviceListOutputFormat::Json : DeviceListOutputFormat::Table;
    PrintDevices(devices, format, std::cout);
    return 0;
}

int RunGuiApplication(int argc, char** argv) {
#ifdef BATTERY_MONITOR_WITH_QT
    QApplication app(argc, argv);
    auto provider = CreateBatteryProvider();
    BatteryWindow window(std::move(provider));
    window.Launch();
    return app.exec();
#else
    (void)argc;
    (void)argv;
    return 0;
#endif
}

}  // namespace

}  // namespace battery_monitor

int battery_monitor::BatteryMonitorMain(int argc, char** argv, bool prefer_gui) {
    const CommandLineOptions options = ParseCommandLine(argc, argv);

    try {
        if (const auto exit_code = TryRunPlatformCommand(options.platform_commands); exit_code.has_value()) {
            return *exit_code;
        }
#ifdef BATTERY_MONITOR_WITH_QT
        if (ShouldLaunchGui(options, prefer_gui)) {
            return RunGuiApplication(argc, argv);
        }
#endif
        return RunCliApplication(options);
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
