# Battery Monitor Audit

Date: 2026-03-19

## Scope

Reviewed:

- `include/`
- `src/`
- `CMakeLists.txt`
- `README.md`
- `AGENTS.md`

Verified on this snapshot:

- `cmake --build build --config Debug --target battery-monitor-app`
- `build\\Debug\\battery-monitor-cli.exe --json`

Smoke output is live and stable on the current Windows machine:

- `DELUX` main `58%`
- `Redmi Buds 4 Pro` right `13%`, case `47%`

## Current Hotspots

- `src/ui/BatteryWindow.cpp` - 2907 lines
- `src/ui/BatteryHistoryDialog.cpp` - 1128 lines
- `src/ui/BatteryStatsDialog.cpp` - 993 lines
- `src/platform/windows/WinRtBatteryProvider.cpp` - 257 lines
- `src/ui/BatteryRuntimeEstimator.cpp` - 766 lines

## Completed Refactor Work

- `main.cpp` is no longer the main CLI parser/formatter monolith. Command parsing, formatting and platform dispatch were moved to `src/app/`.
- Noise control vocabulary is now canonicalized in shared code, so `balanced/balance` and `standard/normal` no longer compete as separate truths.
- UI no longer uses `dynamic_cast<INoiseControlProvider*>`; access is routed through the provider contract.
- `WinRtBatteryProvider.cpp` has already been split into extracted Windows modules:
  - `BluetoothSocketUtils`
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
  - `ClassicBluetoothBatteryFallback`
  - `BluetoothPnpUtils`
  - `BluetoothPnpHints`
  - `ZmiVendorBatteryHints`
  - `WindowsBatteryAggregation`
  - `WindowsBleCandidateBatteryCollector`
  - `WindowsTwsCandidateBatteryCollector`
  - `XiaomiBatteryCaches`
  - `XiaomiControlActions`
- `BatteryWindow.cpp` has started to split into extracted UI modules:
  - `BatteryWindowSettings`
  - `DraggableDeviceRow`

## Remaining P0 Issues

### 1. `BatteryWindow` is now the largest UI god-class

Status: started, but still the main UI god-class.

Symptoms:

- layout, tray, notifications, refresh flow, runtime ETA and ANC UI still live together
- UI state is still spread across many maps, flags and widget properties

Target end-state:

- split state, settings, rendering and interaction services

### 2. Capability truth is only partially fixed

Status: improved, not closed.

What changed:

- the UI capability leak via `dynamic_cast` is gone

What remains:

- `IBluetoothBatteryProvider::GetNoiseControlProvider()` is still a transitional escape hatch
- `core` still lacks a stronger capability-aware app/service contract

### 3. Product baseline and docs still lag behind real code

Status: still open.

Symptoms:

- repo now contains CLI, GUI, app-layer modules and vendor-specific Windows modules
- baseline docs still describe a simpler earlier architecture

## P1 Issues

### 1. Device normalization is still split between provider and UI

- provider merges and dedupes device data
- UI still re-groups and re-selects preferred entries

### 2. `DeviceBatteryInfo` is overloaded

- identity
- battery state
- mode/submode
- visual hints
- cache/connectivity flags

These should not stay in one DTO forever.

### 3. Linux provider still uses blocking D-Bus flow

- `send_with_reply_and_block(..., -1, ...)`
- repeated property plumbing without one typed accessor layer

### 4. CMake still mixes app/platform/UI into one static library

- `battery-monitor-app` is still broader than the architecture it represents

## Double Truth Map

### Closed or mostly closed

- ANC mode/submode vocabulary truth
- UI capability lookup via `dynamic_cast`
- `main.cpp` parser/formatter/platform-dispatch truth

### Still open

- capability truth at `core` boundary
- device normalization truth between provider and UI
- product baseline truth between docs and build
- runtime-state truth in `BatteryWindow`

## Residual Technical Risk

- `BatteryWindow.cpp` is still large enough that unrelated changes can collide
- no automated test suite exists; regression safety is still mostly build + manual smoke

## Recommended Next Work

1. Continue `BatteryWindow` decomposition: alerts/runtime state first, rendering second.
2. Replace the transitional `GetNoiseControlProvider()` escape hatch with a real capability-aware app contract in `core`.
3. Start reducing `BatteryHistoryDialog` and `BatteryStatsDialog`.
4. Split CMake targets into `core`, `platform`, `app`, `ui`.
