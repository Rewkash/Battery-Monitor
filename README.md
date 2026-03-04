# Battery Monitor

Cross-platform C++ project for reading Bluetooth device battery level:

- Windows: WinRT (`Windows.Devices.Bluetooth`)
- Linux: BlueZ over D-Bus (`org.bluez.Battery1`)

Current scope is a minimal native CLI that lists connected devices with battery percentage.

## Features

- Shared C++ core interface for Bluetooth battery providers.
- Native Windows implementation using WinRT GATT Battery Service.
- Native Linux implementation using D-Bus calls to BlueZ.
- JSON output mode for easy integration with scripts.
- Component-aware battery output (`main`, `left`, `right`, `case` when exposed by device/API).

## Requirements

### Windows

- Windows 10/11
- Visual Studio 2022 Build Tools with MSVC + Windows 10/11 SDK
- CMake 3.21+

### Linux

- GCC/Clang with C++20 support
- CMake 3.21+
- `pkg-config`
- `libdbus-1-dev`
- BlueZ running on system bus

## Build

### Windows (Developer PowerShell)

```powershell
cmake --fresh -S . -B build -A x64
cmake --build build --config Debug
.\build\Debug\battery-monitor.exe
```

### Linux

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/battery-monitor
```

## Run

```bash
battery-monitor
battery-monitor --json
```

## Notes

- Not every Bluetooth device exposes battery data through standard Battery Service/Battery1.
- On Linux, `org.bluez.Battery1` availability depends on BlueZ/device support.
- GUI is intentionally not included in this initial baseline. The current architecture is ready for a future Qt-based UI that can run in X11 and Wayland sessions.
