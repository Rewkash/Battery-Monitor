cmake_minimum_required(VERSION 3.21)

project(BatteryMonitor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTORCC ON)
option(BATTERY_MONITOR_ENABLE_QT "Enable Qt6 test GUI" ON)

set(BATTERY_MONITOR_QT_ENABLED OFF)
if(BATTERY_MONITOR_ENABLE_QT)
    find_package(Qt6 COMPONENTS Widgets Svg QUIET)
    if(Qt6Widgets_FOUND AND Qt6Svg_FOUND)
        set(BATTERY_MONITOR_QT_ENABLED ON)
        message(STATUS "Qt6 Widgets and Svg found. GUI mode enabled.")
    else()
        message(WARNING "Qt6 Widgets/Svg not found. Building CLI-only binary.")
    endif()
endif()

set(BATTERY_MONITOR_APP_SOURCES
    src/app/BatteryOutputFormatter.cpp
    src/app/CommandLineOptions.cpp
    src/core/NoiseControlVocabulary.cpp
    src/main.cpp
    src/core/ProviderFactory.cpp
)

add_library(
    battery-monitor-app
    STATIC
    ${BATTERY_MONITOR_APP_SOURCES}
)

target_include_directories(
    battery-monitor-app
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(
        battery-monitor-app
        PRIVATE
            /W4
            /permissive-
            /EHsc
            /utf-8
    )
else()
    target_compile_options(
        battery-monitor-app
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )
endif()

if(WIN32)
    target_sources(
        battery-monitor-app
        PRIVATE
            src/platform/windows/devices/phone/BluetoothPnpHints.cpp
            src/platform/windows/bluetooth/BluetoothPnpUtils.cpp
            src/platform/windows/bluetooth/BluetoothSocketUtils.cpp
            src/platform/windows/shared/WindowsBluetoothAddressUtils.cpp
            src/platform/windows/shared/WindowsBatteryAggregation.cpp
            src/platform/windows/shared/WindowsBleCandidateBatteryCollector.cpp
            src/platform/windows/shared/WindowsBatteryEntryUtils.cpp
            src/platform/windows/shared/WindowsBatteryProviderSupport.cpp
            src/platform/windows/shared/WindowsBatteryQueryReaders.cpp
            src/platform/windows/shared/WindowsTwsCandidateBatteryCollector.cpp
            src/platform/windows/shared/BatteryComponentNaming.cpp
            src/platform/windows/bluetooth/BleCandidateEnumeration.cpp
            src/platform/windows/bluetooth/BleStandardBatteryReader.cpp
            src/platform/windows/bluetooth/BleVendorTripletReader.cpp
            src/platform/windows/devices/xiaomi/ClassicBluetoothBatteryFallback.cpp
            src/platform/windows/PlatformCommandDispatcher.cpp
            src/platform/windows/devices/controller/WindowsControllerBatteryReader.cpp
            src/platform/windows/shared/WindowsDeviceInfoProperties.cpp
            src/platform/windows/devices/xiaomi/XiaomiAuth.cpp
            src/platform/windows/devices/xiaomi/XiaomiBatteryCaches.cpp
            src/platform/windows/devices/xiaomi/XiaomiBatteryCodec.cpp
            src/platform/windows/devices/xiaomi/XiaomiBatteryReadings.cpp
            src/platform/windows/devices/xiaomi/XiaomiClassicBatterySession.cpp
            src/platform/windows/devices/xiaomi/XiaomiControlActions.cpp
            src/platform/windows/devices/xiaomi/XiaomiControlConnection.cpp
            src/platform/windows/devices/xiaomi/XiaomiControlSession.cpp
            src/platform/windows/devices/xiaomi/XiaomiControlSocket.cpp
            src/platform/windows/devices/xiaomi/XiaomiHandshake.cpp
            src/platform/windows/devices/xiaomi/XiaomiModeCache.cpp
            src/platform/windows/devices/xiaomi/XiaomiNoiseModeCodec.cpp
            src/platform/windows/devices/xiaomi/XiaomiPersistentCache.cpp
            src/platform/windows/devices/xiaomi/XiaomiProtocol.cpp
            src/platform/windows/WinRtBatteryProvider.cpp
    )

    target_compile_definitions(
        battery-monitor-app
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
    )

    target_link_libraries(
        battery-monitor-app
        PUBLIC
            Cfgmgr32
            Hid
            Setupapi
            windowsapp
            Ws2_32
    )
elseif(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DBUS REQUIRED dbus-1)

    target_sources(
        battery-monitor-app
        PRIVATE
            src/platform/linux/PlatformCommandDispatcher.cpp
            src/platform/linux/BluezBatteryProvider.cpp
    )

    target_include_directories(
        battery-monitor-app
        PRIVATE
            ${DBUS_INCLUDE_DIRS}
    )

    target_link_libraries(
        battery-monitor-app
        PUBLIC
            ${DBUS_LIBRARIES}
    )

    target_compile_options(
        battery-monitor-app
        PRIVATE
            ${DBUS_CFLAGS_OTHER}
    )
else()
    message(FATAL_ERROR "This project currently supports only Windows and Linux.")
endif()

if(BATTERY_MONITOR_QT_ENABLED)
    target_sources(
        battery-monitor-app
        PRIVATE
            src/ui/BatteryHistoryDialog.cpp
            src/ui/BatteryHistoryDialog.h
            src/ui/BatteryRuntimeEstimator.cpp
            src/ui/BatteryRuntimeEstimator.h
            src/ui/BatteryHistoryStore.cpp
            src/ui/BatteryHistoryStore.h
            src/ui/BatteryStatsDialog.cpp
            src/ui/BatteryStatsDialog.h
            src/ui/DraggableDeviceRow.cpp
            src/ui/DraggableDeviceRow.h
            src/ui/NoiseControlUi.cpp
            src/ui/NoiseControlUi.h
            src/ui/BatteryWindow.cpp
            src/ui/BatteryWindow.h
            src/ui/BatteryWindowSettings.cpp
            src/ui/BatteryWindowSettings.h
            resources/device_icons.qrc
    )

    target_compile_definitions(
        battery-monitor-app
        PRIVATE
            BATTERY_MONITOR_WITH_QT
    )

    target_link_libraries(
        battery-monitor-app
        PUBLIC
            Qt6::Widgets
            Qt6::Svg
    )

endif()

if(BATTERY_MONITOR_QT_ENABLED)
    if(WIN32)
        add_executable(battery-monitor WIN32 src/gui_main.cpp)
    else()
        add_executable(battery-monitor src/gui_main.cpp)
    endif()
    add_executable(battery-monitor-cli src/cli_main.cpp)

    target_link_libraries(battery-monitor PRIVATE battery-monitor-app)
    target_link_libraries(battery-monitor-cli PRIVATE battery-monitor-app)

    if(WIN32)
        get_target_property(_qt_qmake_executable Qt6::qmake IMPORTED_LOCATION)
        get_filename_component(_qt_bin_dir "${_qt_qmake_executable}" DIRECTORY)
        find_program(WINDEPLOYQT_EXECUTABLE
            NAMES windeployqt windeployqt.exe
            HINTS "${_qt_bin_dir}"
            REQUIRED
        )

        add_custom_command(
            TARGET battery-monitor
            POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                    --no-compiler-runtime
                    --no-translations
                    "$<TARGET_FILE:battery-monitor>"
            COMMENT "Deploying Qt runtime for battery-monitor.exe"
            VERBATIM
        )
    endif()
else()
    add_executable(battery-monitor src/cli_main.cpp)
    target_link_libraries(battery-monitor PRIVATE battery-monitor-app)
endif()

