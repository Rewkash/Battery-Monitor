# ChargeView

[Русская версия](README.md)

Native C++20 application for viewing Bluetooth device battery state.

The current application version is defined once in the root `CMakeLists.txt`. Windows builds include an optional signed GitHub Releases updater; see [auto-update design](docs/auto-update-design.md) and [release process](docs/release-process.md).

## Product naming

The product uses two distinct names on purpose:

- **Display name: `ChargeView`** — the human-readable name used in UI titles, dialogs, the installer wizard, shortcuts and release communication. Defined as `BATTERY_MONITOR_DISPLAY_NAME` (with `BATTERY_MONITOR_PUBLISHER`) in `CMakeLists.txt`; installer metadata derives its display strings from there.
- **Internal identity: `BatteryMonitor` / `battery-monitor`** — the stable machine identity: the CMake project name, target names (`battery-monitor`, `battery-monitor-cli`, `battery-monitor-maintenance`), executable and artifact file names (`battery-monitor-vX.Y.Z-win-x64.{msi,zip,bmup}`), the MSI UpgradeCode, registry keys under `Software\Orion Group\Battery Monitor`, and QSettings organization/application names.

The internal identity is frozen. Never rename targets, executables, installer paths, registry keys or settings identifiers: upgrades, the auto-updater and user settings depend on them. Only human-readable strings may follow the display name.

## Installation

For first-time installation on Windows, download the x64 MSI from [GitHub Releases](https://github.com/Rewkash/Battery-Monitor/releases). The installer is per-user (no administrator rights required), remembers the selected folder across upgrades, and when a drive root is picked it automatically appends the `ChargeView` folder (for example, `H:\` → `H:\ChargeView`). To upgrade, simply run the new MSI over the existing installation: it closes the running app, keeps the install folder and replaces files in place.

The ZIP remains available for portable use, and the `.bmup` asset is used internally by portable automatic updates.

Version and update check:

```powershell
.\build\Debug\battery-monitor-cli.exe --version
.\build\Debug\battery-monitor-cli.exe --check-updates --json
```

## Features

- Lists Bluetooth devices with battery level when available.
- Supports component-aware battery readings: `main`, `left`, `right`, and `case`.
- Marks cached/offline readings separately from live values.
- Plain text and JSON CLI output; invalid arguments produce a clear usage message and a non-zero exit code.
- Builds an optional Qt Widgets UI when Qt 6 is available.
- Low-battery notifications with a per-physical-device threshold and repeat interval.
- 14-day charge history with offline events, statistics and runtime estimation (ETA).
- Loads JSON device profiles from `profiles/devices` for device-family matching.
- Windows-specific support for BLE devices, phones, controllers, and Xiaomi/Redmi TWS devices.

## Requirements

### Windows

- Windows 10/11
- Visual Studio 2022+ Build Tools with MSVC
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

- `--version` — print the version and exit.
- `--json` — machine-readable device data.
- `--all`, `--include-offline` — include offline/cached devices.
- `--cli` / `--gui` — force CLI output or the graphical application.
- `--check-updates` — check for application updates and exit.
- `--xiaomi-set-noise <mode> [device]` — set the Xiaomi noise-control mode.
- `--xiaomi-set-submode <family> <index> [device]` — set a Xiaomi noise-control submode.

Invalid arguments (unknown flags, missing values, incompatible `--gui`/`--cli`/`--json`) produce a usage message and exit code 2.

## Device Profiles

Device profiles live under `profiles/devices` and are regular JSON files. They extend device-family/category matching without recompiling C++ heuristics.

Profile lookup order:

- `BATTERY_MONITOR_PROFILE_DIR`, if set
- `profiles/devices` in the current working directory
- `profiles/devices` in parent directories of the current working directory

The schema, specificity rules and validation are documented in `profiles/devices/README.md`.

## Architecture

Important source areas:

- `include/core`: public domain types and provider interfaces.
- `src/app`: command-line parsing, output formatting, and application entry helpers.
- `src/core`: shared core implementations.
- `src/platform/windows`: Windows provider, WinRT integration, and Windows-specific device readers.
- `src/platform/linux`: Linux BlueZ provider.
- `src/ui`: Qt Widgets UI (window, history, ETA, notifications).
- `src/update`: update service, manifest signature, maintenance tool.
- `profiles/devices`: data-driven device profiles.
- `assets` and `resources`: UI assets and Qt resources.

Platform-specific Bluetooth code stays under its platform folder. Shared code should not include WinRT, Windows headers, BlueZ, or D-Bus headers directly.

## Notes

- Not every Bluetooth device exposes battery data through a standard API.
- On Linux, `org.bluez.Battery1` availability depends on BlueZ and device support.
- On Windows, some devices require fallback readers or cached values because the standard GATT Battery Service is not always exposed.
- Device control support is currently platform- and device-specific.
