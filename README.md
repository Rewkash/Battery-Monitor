# ChargeView

Current application version is defined once in the root `CMakeLists.txt`. Windows builds include an optional signed GitHub Releases updater; see [auto-update design](docs/auto-update-design.md) and [release process](docs/release-process.md).

For first-time installation, download the Windows x64 ZIP from [GitHub Releases](https://github.com/Rewkash/Battery-Monitor/releases), extract it into a writable directory, and run `battery-monitor.exe`. The `.bmup` asset is used internally by automatic updates.

Version and update check:

```powershell
.\build\Debug\battery-monitor-cli.exe --version
.\build\Debug\battery-monitor-cli.exe --check-updates --json
```

Native C++20 application for viewing Bluetooth device battery state.

The project provides a shared core, a CLI, and an optional Qt 6 desktop UI. Windows uses WinRT Bluetooth APIs with extra device-specific readers for several common device classes. Linux reads battery data from BlueZ over D-Bus.

## Features

- Lists Bluetooth devices with battery level when available.
- Supports component-aware battery readings: `main`, `left`, `right`, and `case`.
- Marks cached/offline readings separately from live values.
- Provides plain text and JSON CLI output.
- Builds an optional Qt Widgets UI when Qt 6 is available.
- Loads JSON device profiles from `profiles/devices` for device-family matching.
- Includes Windows-specific support for BLE devices, phones, controllers, and Xiaomi/Redmi TWS devices.

## Requirements

### Windows

- Windows 10/11
- Visual Studio 2022 Build Tools with MSVC
- Windows 10/11 SDK
- CMake 3.21+
- Qt 6 Widgets and Svg modules for the GUI build, optional

### Linux

- GCC or Clang with C++20 support
- CMake 3.21+
- `pkg-config`
- `libdbus-1-dev`
- BlueZ running on the system bus
- Qt 6 Widgets and Svg modules for the GUI build, optional

## Build

### Windows

```powershell
cmake --fresh -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

When Qt 6 is found, the build creates:

- `build\Debug\battery-monitor.exe` for the GUI
- `build\Debug\battery-monitor-cli.exe` for the CLI

Without Qt 6, `battery-monitor.exe` is built as the CLI executable.

### Linux

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

When Qt 6 is found, the build creates both `battery-monitor` and `battery-monitor-cli`. Without Qt 6, `battery-monitor` is the CLI executable.

To force a CLI-only build on any platform:

```bash
cmake -S . -B build -DBATTERY_MONITOR_ENABLE_QT=OFF
```

## Run

```bash
battery-monitor
battery-monitor-cli --json
```

Useful CLI options:

- `--json` prints machine-readable device data.
- `--debug` enables diagnostic logging where supported.

## Device Profiles

Device profiles live under `profiles/devices` and are regular JSON files. They are used to extend device-family/category matching without recompiling C++ heuristics.

Profile lookup order:

- `BATTERY_MONITOR_PROFILE_DIR`, if set
- `profiles/devices` in the current working directory
- `profiles/devices` in parent directories of the current working directory

See `profiles/devices/README.md` for the schema example.

## Architecture

Important source areas:

- `include/core`: public domain types and provider interfaces.
- `src/app`: command-line parsing, output formatting, and application entry helpers.
- `src/core`: shared core implementations.
- `src/platform/windows`: Windows provider, WinRT integration, and Windows-specific device readers.
- `src/platform/linux`: Linux BlueZ provider.
- `src/ui`: Qt Widgets UI.
- `profiles/devices`: data-driven device profiles.
- `assets` and `resources`: UI assets and Qt resources.

Platform-specific Bluetooth code stays under its platform folder. Shared code should not include WinRT, Windows headers, BlueZ, or D-Bus headers directly.

## Notes

- Not every Bluetooth device exposes battery data through a standard API.
- On Linux, `org.bluez.Battery1` availability depends on BlueZ and device support.
- On Windows, some devices require fallback readers or cached values because the standard GATT Battery Service is not always exposed.
- Device control support is currently platform- and device-specific.
