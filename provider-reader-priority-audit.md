# Provider Reader Priority Audit

Date: 2026-03-19

Goal: re-check extracted Windows modules from the perspective of newer and better battery readers, not just from raw call-count.

## Main conclusion

Yes: there is a lot of code that is **not primary anymore**.

But that does **not** mean it is dead.

In the current codebase the extracted Windows logic splits into 4 groups:

1. `primary reader path`
2. `secondary / weak fallback path`
3. `cold legacy path`
4. `debug / experimental path`

The important distinction:

- `not primary` != `safe to delete`
- many old readers are now only fallback layers
- some of those fallback layers are still needed on the current machine

## Current priority order

### Primary path

These are the best readers today and should be treated as the canonical path:

- `ReadConnectedBluetoothDeviceBatteryFast`
- `ReadAssociationEndpointBattery`
- `ReadBleBatteryReadings`
- `TryReadBleVendorTripletBattery`
- `ReadBatteryPercentFromEndpointProperties`
- `ReadZmiVendorBatteryHint*`

Meaning:

- fast connected-device query is preferred over slow generic enumeration
- standard BLE / vendor triplet / endpoint properties are preferred over classic RFCOMM/HFP tricks
- ZMI vendor property hints are preferred over serial probing when available

## What is now fallback only

### 1. `ReadGenericDeviceBattery`

Status: `cold fallback`

Why:

- in `WinRtBatteryProvider::GetDevicesBattery` it now runs only if:
  - `BATTERY_MONITOR_GENERIC_SCAN=1`, or
  - the accumulator is empty

Implication:

- this is no longer part of the normal happy path
- it is already a last-resort enumeration layer

### 2. `ReadAssociationEndpointBattery`

Status: `secondary fallback`

Why:

- `CollectTwsCandidateBatteryEntries` first calls `ReadConnectedBluetoothDeviceBatteryFast`
- AEP scan runs only if forced or if fast scan found too few TWS candidates

Implication:

- useful, but no longer the first source of truth

### 3. `TryReadGenericClassicHfpBattery`

Status: `weak fallback`

Why:

- in `ReadConnectedBluetoothDeviceBatteryFast` phone battery logic tries:
  1. `ReadPhoneHfpBatteryHintFromPnpAddress`
  2. only then `TryReadGenericClassicHfpBattery`

Implication:

- HFP classic read is already second-line fallback for phones

### 4. `TryReadXiaomiClassicBattery`

Status: `legacy fallback`, but still important

Why:

- newer BLE/vendor/property readers exist
- however for Xiaomi/Redmi TWS devices this path is still used when richer BLE data is missing
- in TWS flow it still participates before or alongside later BLE/vendor fallback resolution

Important:

- on the current machine the Redmi path still falls back to cached/classic-derived data
- so this is old code, but not dead code

### 5. `TryReadZmiSerialBatteryFromSocket`

Status: `deep legacy fallback`

Why:

- it is reached only inside classic Xiaomi/ZMI RFCOMM fallback logic
- newer ZMI vendor hint/property readers are tried earlier in query readers

Implication:

- this is a real candidate for future optional removal
- but only if you are willing to drop some ZMI/PurPods edge cases

### 6. `HfpBatterySession`

Status: `deep legacy fallback`

Why:

- HFP parsing now sits behind:
  - endpoint battery props
  - phone PnP hint
  - BLE paths
  - some Xiaomi/ZMI readers

Implication:

- definitely not modern primary logic
- still useful as compatibility tail

### 7. `XiaomiPersistentCache` / persisted Xiaomi snapshots

Status: `recovery fallback`

Why:

- persistent snapshots are only used if live read quality is weak or connection paths fail

Important:

- on the current machine this path is still visibly active for `Redmi Buds 4 Pro`
- so from architecture perspective it is fallback
- from practical behavior perspective it is still covering real failures

## What is not part of battery reading at all

These modules are easy to mistake for “reader bloat”, but they are actually diagnostics/control surface, not battery-read surface:

- `XiaomiControlActions`
- `BleGattDump`
- `RfcommServiceDump`
- `ProbeXiaomiNoiseControl`
- `ObserveXiaomiControlSession`
- `ObserveZmiSerialSession`
- `SendXiaomiControlCandidate`

Status: `debug / experimental`

Implication:

- if your goal is to simplify battery reading only, these are not the first files to blame
- they are removable only together with CLI diagnostic / experimental commands

## Real removal candidates

From the perspective of “better readers now exist”, the best candidates are:

### Tier A: safest to isolate or feature-flag

- `ReadGenericDeviceBattery`
  Reason: already cold path.

- `TryReadGenericClassicHfpBattery`
  Reason: weak compatibility fallback, no longer primary.

- `TryReadZmiSerialBatteryFromSocket`
  Reason: very deep fallback behind newer ZMI readers.

- `HfpBatterySession`
  Reason: compatibility tail only.

### Tier B: removable only if you accept feature / device loss

- `TryReadXiaomiClassicBattery`
  Reason: architecturally old, but still practically important for Xiaomi edge cases.

- `XiaomiPersistentCache`
  Reason: ugly fallback, but still currently saving real failures.

- `ReadAssociationEndpointBattery`
  Reason: no longer first path, but still useful when fast scan misses TWS candidates.

### Tier C: not reader cleanup, but command-surface cleanup

- `XiaomiControlActions`
- `BleGattDump`
- `RfcommServiceDump`

Reason:

- these are not “old data readers”
- they are optional diagnostics / experimental controls

## What I would call truly suspicious

If the goal is “remove the old stuff because better readers were written later”, the most suspicious pieces are not whole files but these exact paths:

1. Generic scan fallback in `ReadGenericDeviceBattery`
2. Phone classic HFP fallback after PnP hint miss
3. ZMI serial probe inside `ClassicBluetoothBatteryFallback`
4. HFP fallback tail inside `ClassicBluetoothBatteryFallback`

These are the places where old transport/protocol logic is clearly trailing behind newer readers.

## What I would NOT remove yet

I would **not** delete these yet:

- `XiaomiBatteryCaches`
- `TryReadXiaomiClassicBattery`
- `XiaomiPersistentCache`

Reason:

- the current machine still shows Xiaomi fallback behavior in real smoke runs
- so these paths are legacy, but still operationally relevant

## Practical next cleanup plan

If the objective is to cut the real legacy reader debt without breaking current behavior, the right order is:

1. Mark `ReadGenericDeviceBattery` as optional legacy fallback in docs/comments.
2. Move HFP/ZMI serial fallback behind explicit feature flags or compile-time options.
3. Add telemetry/debug counters for which reader actually won for each device.
4. Only after live evidence, remove:
   - generic HFP fallback
   - ZMI serial fallback
   - maybe parts of classic RFCOMM fallback

## Bottom line

From this perspective you were right:

- a noticeable part of the extracted provider code is now old compatibility logic
- but most of it is not garbage; it is fallback compatibility code
- the biggest “looks old and maybe removable” area is:
  - `ClassicBluetoothBatteryFallback`
  - `HfpBatterySession`
  - `ZmiSerialBatterySession`
  - `ReadGenericDeviceBattery`

The one thing that still blocks aggressive removal is simple:

- current Xiaomi/Redmi behavior still relies on fallback paths
