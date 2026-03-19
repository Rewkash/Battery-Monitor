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
            src/platform/windows/BluetoothPnpHints.cpp
            src/platform/windows/BluetoothPnpUtils.cpp
            src/platform/windows/BluetoothSocketUtils.cpp
            src/platform/windows/WindowsBluetoothAddressUtils.cpp
            src/platform/windows/WindowsBatteryAggregation.cpp
            src/platform/windows/WindowsBleCandidateBatteryCollector.cpp
            src/platform/windows/WindowsBatteryEntryUtils.cpp
            src/platform/windows/WindowsBatteryProviderSupport.cpp
            src/platform/windows/WindowsBatteryQueryReaders.cpp
            src/platform/windows/WindowsTwsCandidateBatteryCollector.cpp
            src/platform/windows/BatteryComponentNaming.cpp
            src/platform/windows/BleGattDump.cpp
            src/platform/windows/BleCandidateEnumeration.cpp
            src/platform/windows/BleStandardBatteryReader.cpp
            src/platform/windows/BleVendorTripletReader.cpp
            src/platform/windows/ClassicBluetoothBatteryFallback.cpp
            src/platform/windows/HfpBatterySession.cpp
            src/platform/windows/PlatformCommandDispatcher.cpp
            src/platform/windows/RfcommServiceDump.cpp
            src/platform/windows/WindowsControllerBatteryReader.cpp
            src/platform/windows/WindowsDeviceInfoProperties.cpp
            src/platform/windows/XiaomiAdvertisementSnapshots.cpp
            src/platform/windows/XiaomiAuth.cpp
            src/platform/windows/XiaomiBatteryCaches.cpp
            src/platform/windows/XiaomiBatteryCodec.cpp
            src/platform/windows/XiaomiBatteryReadings.cpp
            src/platform/windows/XiaomiClassicBatterySession.cpp
            src/platform/windows/XiaomiControlActions.cpp
            src/platform/windows/XiaomiControlConnection.cpp
            src/platform/windows/XiaomiControlSession.cpp
            src/platform/windows/XiaomiControlSocket.cpp
            src/platform/windows/XiaomiHandshake.cpp
            src/platform/windows/XiaomiModeCache.cpp
            src/platform/windows/XiaomiNoiseModeCodec.cpp
            src/platform/windows/XiaomiPersistentCache.cpp
            src/platform/windows/XiaomiProtocol.cpp
            src/platform/windows/WinRtBatteryProvider.cpp
            src/platform/windows/ZmiBatteryCodec.cpp
            src/platform/windows/ZmiVendorBatteryHints.cpp
            src/platform/windows/ZmiSerialBatterySession.cpp
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
