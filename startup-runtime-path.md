# Runtime Path: `battery-monitor-cli.exe --json`

Trace date: `2026-03-19`  
Environment: Windows, local dev machine  
Command:

```powershell
$env:BATTERY_MONITOR_DEBUG='1'
.\build\Debug\battery-monitor-cli.exe --json
```

This file describes what was confirmed by live traces, not every branch that can exist in code.
The mapping is based on:
- runtime debug log;
- current orchestration flow in source files.

Recheck status:
- the trace was repeated;
- the high-level startup path stayed the same;
- device counts and battery values changed between runs, which is expected;
- one more confirmed live branch appeared in the second check: phone PnP battery hint for `POCO F3`.

## 1. Entry chain

Confirmed startup chain:

1. `battery_monitor::BatteryMonitorMain`
2. `ParseCommandLine`
3. `TryRunPlatformCommand`
4. `RunCliApplication`
5. `CreateBatteryProvider`
6. `WinRtBatteryProvider::GetDevicesBattery`

Meaning for `--json`:
- GUI path was not used;
- platform command path was not used;
- the app went through the normal CLI battery query path.

## 2. Confirmed executed in this run

### CLI / app layer

- `battery_monitor::BatteryMonitorMain`
- `ParseCommandLine`
- `TryRunPlatformCommand`
- `RunCliApplication`
- `CreateBatteryProvider`
- `PrintDevices`

### Provider entry

- `WinRtBatteryProvider::GetDevicesBattery`
- `EnsureWindowsBatteryProviderApartmentInitialized`
- `GetWindowsBatteryProviderRuntimeOptions`
- `MakeWindowsBatteryQueryReaderContext`
- `MakeWindowsBleCandidateBatteryCollectorContext`
- `MakeWindowsTwsCandidateBatteryCollectorContext`

### BLE candidate path

- `CollectBleCandidateBatteryEntries`
- `EnumerateBleCandidateDevices`
- `ReadBleBatteryReadings`

Confirmed by log:
- `BLE candidates from selectors: 2`
- `BLE candidate open succeeded ...`
- `BLE candidate battery read took ...`

Observed effect:
- `DELUX` was read by the standard BLE battery path;
- the second BLE candidate returned `0` standard battery entries and then moved into fallback paths.

### Fast connected-device / TWS path

- `CollectTwsCandidateBatteryEntries`
- `ReadConnectedBluetoothDeviceBatteryFast`

Confirmed by log:
- `Fast connected-device query took 30 ms` in the first trace and `31 ms` in the recheck
- `Fast connected-device entries scanned: 1` in the first trace and `2` in the recheck
- `Fast connected battery entries: 2` in the first trace and `3` in the recheck
- `Fast TWS candidates: 2`

Second recheck additionally confirmed the phone hint branch inside fast query:

- `ReadPhoneHfpBatteryHintFromPnpAdapter`
- `ReadPhoneHfpBatteryHintFromPnpAddress`

Confirmed by log:
- `Phone PnP raw pid=2 type=3 size=1 data=3C`
- `Phone PnP battery hint accepted value=60 ...`

Conclusion:
- `POCO F3` battery in the fast-query path was read through the PnP phone hint branch;
- in that run the generic classic HFP fallback was not needed for that phone path.

### Xiaomi classic fallback path

- `XiaomiClassicBatteryCache::Read`
- `TryReadXiaomiClassicBattery`

Confirmed by log:
- `Xiaomi classic fallback: attempting RFCOMM connection ...`
- `Classic RFCOMM: connected via FD2D`
- multiple `Xiaomi message ...`
- `Xiaomi classic fallback: battery merged left=100 right=na case=36` in the first trace
- `Xiaomi classic fallback: battery merged left=97 right=na case=36` in the recheck

Conclusion:
- classic Xiaomi fallback is not dead;
- it is actively used on this machine and succeeded in this run.

### Xiaomi advertisement fallback path

- `XiaomiAdvertisementBatteryCache::Read`
- `ScanXiaomiAdvertisementSnapshots`

Confirmed by log:
- after BLE/classic failure for the second TWS candidate, advertisement scan ran:
  - `BLE advertisement fallback scanned in 2499 ms (requested=1800), candidate addresses=5 names=2`

Conclusion:
- advertisement fallback is also an active runtime path.

### Final result shaping

- `ApplyPnpVisualHints`
- `FinalizeCollectedEntries`

This is the stage after which CLI prints the final JSON.

## 3. Confirmed skipped in this run

### AEP path

- `ReadAssociationEndpointBattery`

Confirmed by log:
- `AEP scan skipped because fast candidate scan already found targets.`

This means the function is cold in this session, not globally dead.

### Generic scan path

- `ReadGenericDeviceBattery`

Confirmed by log:
- `Generic device scan skipped (set BATTERY_MONITOR_GENERIC_SCAN=1 to enable).`

Reason:
- `generic_scan_enabled == false`
- and `device_accumulator` was already non-empty.

### Disconnected paired fallback

- `CollectDisconnectedPairedBluetoothEntries`

Reason:
- this run did not use `--all`
- so `options.include_disconnected == false`

### Control / diagnostics commands

Not used:

- `ProbeXiaomiNoiseControl`
- `ObserveXiaomiControlSession`
- `ObserveZmiSerialSession`
- `DumpBluetoothServices`
- `DumpBleGatt`
- `SetXiaomiNoiseMode`
- `SetNoiseControlMode`
- `SendXiaomiControlCandidate`
- `SetXiaomiNoiseSubmode`

Reason:
- `TryRunPlatformCommand` returned `std::nullopt` because `--json` did not set any platform command flags.

### GUI path

Not used:

- `RunGuiApplication`
- `BatteryWindow` and the Qt UI flow

Reason:
- this was a CLI run.

## 4. What this says about "extra code"

Practical conclusions:

- `WinRtBatteryProvider` is now a thin orchestrator and the startup path really goes through extracted modules.
- Old Xiaomi-related code is not fully dead: classic RFCOMM fallback and advertisement fallback are both active at runtime.
- `ReadAssociationEndpointBattery` and `ReadGenericDeviceBattery` were skipped in this session, but that alone is not enough to delete them.
- If cleanup continues from the perspective of "what actually reads data first", the best next review targets are the branches that keep getting skipped:
  - `ReadAssociationEndpointBattery`
  - `ReadGenericDeviceBattery`
  - parts of generic/classic non-Xiaomi fallback

## 5. Actual result of this trace

CLI output in this run:

- `DELUX` -> `58%`
- `Redmi Buds 4 Pro` -> `left 97%`, `case 36%`, `deviceMode=off`
- `POCO F3` -> `60%`

This matters because it shows:
- the standard BLE path is alive;
- Xiaomi classic fallback is alive;
- the fast-query phone hint path is alive;
- current success is not coming only from persisted cache.
