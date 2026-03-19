# Battery Monitor Context

Date: 2026-03-19

## Product Purpose

`Battery Monitor` is a native cross-platform C++ application that reads Bluetooth device battery state.

Current reality:

- Windows is supported
- Linux is supported
- CLI exists
- Qt GUI exists
- Windows also contains vendor-specific Xiaomi/ZMI control and diagnostics paths

## User Value

The product answers:

- which Bluetooth devices are visible
- whether battery data is available
- what the battery level is
- whether the shown value is live, cached or offline

For supported Windows devices it also exposes noise-control state and controls.

## Current Entry Points

- `src/cli_main.cpp`
- `src/gui_main.cpp`
- shared app-layer helpers in `src/app/`

`main.cpp` is no longer supposed to own parsing and formatting logic directly.

## Domain Semantics

### Device identity

The stable user-facing identity is built from:

- `device_id`
- `device_name`

### Battery components

Canonical component tokens:

- `main`
- `left`
- `right`
- `case`

`main` means either a single-battery device or a device where finer component detail is not available.

### Battery value

- valid range: `0..100`
- missing value means battery is unavailable or not readable in the current pass

### Data freshness

- `is_cached = false` means live/current read
- `is_cached = true` means cache or fallback-derived read
- `is_connected` is separate from `is_cached`

An offline device may still have a cached battery value.

### Noise-control semantics

Canonical mode tokens:

- `off`
- `anc`
- `transparency`

Canonical submode examples:

- ANC: `balanced`, `weak`, `deep`, `adaptive`
- Transparency: `standard`, `voice`

Wire tokens and UI labels must not become separate truths.

## Data Sources

### Windows

Primary sources:

- WinRT Bluetooth APIs

Fallback and enrichment sources:

- standard GATT Battery Service
- BLE endpoint scan
- generic classic RFCOMM/HFP fallback
- PnP and registry metadata
- Xiaomi/ZMI vendor protocol
- in-memory cache
- persistent Xiaomi cache

### Linux

Primary source:

- BlueZ over D-Bus via `org.bluez.Battery1`

Important semantic rule:

- `Device1` does not imply `Battery1`

## Actual Windows Module Map

The Windows provider is no longer a single implementation blob. Important extracted modules now include:

- `BluetoothSocketUtils`
- `ClassicBluetoothBatteryFallback`
- `BluetoothPnpUtils`
- `BluetoothPnpHints`
- `ZmiVendorBatteryHints`
- `XiaomiProtocol`
- `XiaomiAuth`
- `XiaomiHandshake`
- `XiaomiControlSession`
- `XiaomiBatteryCodec`
- `XiaomiBatteryReadings`
- `XiaomiClassicBatterySession`
- `XiaomiModeCache`
- `XiaomiNoiseModeCodec`
- `XiaomiPersistentCache`
- `ZmiBatteryCodec`
- `ZmiSerialBatterySession`
- `HfpBatterySession`

`WinRtBatteryProvider` should keep moving toward orchestration-only responsibility.

## Business Invariants

- The app must not crash when a device does not expose battery data.
- Unsupported capability must be reported as unavailable, not as a fatal error.
- Platform code must stay in platform folders.
- JSON output is a compatibility contract and must remain stable unless migrated intentionally.
- UI and CLI should consume interfaces and app-layer contracts, not concrete platform classes.

## What Is Not Business Logic

Do not treat these as domain truth:

- Qt styling and widget layout
- tray animation details
- debug env flags
- Windows diagnostic commands
- concrete visual card arrangement

These are presentation or technical details.

## Main Remaining Architecture Risks

- `WinRtBatteryProvider.cpp` is still too large and still mixes orchestration with policy.
- `BatteryWindow.cpp` is the biggest UI god-class.
- `DeviceBatteryInfo` carries too many concerns in one DTO.
- Linux provider still needs typed D-Bus helpers and timeout policy.
- CMake targets still do not mirror the intended architecture.

## Target Truth Order

The intended truth order for future changes is:

1. `include/core` for domain types and capability contracts
2. `src/platform/*` for OS-specific implementation
3. app layer for orchestration/use-cases
4. CLI and GUI for presentation

Never the other way around.
