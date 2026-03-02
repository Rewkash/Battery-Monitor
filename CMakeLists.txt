cmake_minimum_required(VERSION 3.21)

project(BatteryMonitor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(
    battery-monitor
    src/main.cpp
    src/core/ProviderFactory.cpp
)

target_include_directories(
    battery-monitor
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(
        battery-monitor
        PRIVATE
            /W4
            /permissive-
            /EHsc
    )
else()
    target_compile_options(
        battery-monitor
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )
endif()

if(WIN32)
    target_sources(
        battery-monitor
        PRIVATE
            src/platform/windows/WinRtBatteryProvider.cpp
    )

    target_compile_definitions(
        battery-monitor
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
    )

    target_link_libraries(
        battery-monitor
        PRIVATE
            windowsapp
    )
elseif(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DBUS REQUIRED dbus-1)

    target_sources(
        battery-monitor
        PRIVATE
            src/platform/linux/BluezBatteryProvider.cpp
    )

    target_include_directories(
        battery-monitor
        PRIVATE
            ${DBUS_INCLUDE_DIRS}
    )

    target_link_libraries(
        battery-monitor
        PRIVATE
            ${DBUS_LIBRARIES}
    )

    target_compile_options(
        battery-monitor
        PRIVATE
            ${DBUS_CFLAGS_OTHER}
    )
else()
    message(FATAL_ERROR "This project currently supports only Windows and Linux.")
endif()

