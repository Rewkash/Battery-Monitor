# Extracted Provider Functions Audit

Date: 2026-03-19

Scope: API that was moved out of `src/platform/windows/WinRtBatteryProvider.cpp`.

## Summary

- `WinRtBatteryProvider.cpp` is no longer the owner of protocol/read/control/cache logic.
- I did **not** find dead exported entry points with zero callers among the current extracted API.
- I **did** find several symbols that are still exposed in headers even though they are effectively private to one module family or one caller.

Status legend:

- `active` - used from several files / stable extracted surface
- `single-caller` - used, but effectively by one consumer only
- `privatize-candidate` - used, but should probably stop being part of header/API surface

## Extracted API Map

### 1. Socket / transport

Module: `BluetoothSocketUtils`

- `SendAll`, `ReceiveChunk`, `ConnectWithTimeout`, `DiscoverRfcommChannelsFromSdp`
  Purpose: low-level Winsock/RFCOMM helpers for Xiaomi/ZMI/RFCOMM flows.
  Status: `active`
  Notes: these are reused across several Windows protocol modules and should stay extracted.

- `ScopedWsa`
  Purpose: RAII wrapper for `WSAStartup/WSACleanup`.
  Status: `active`

- `DiscoveredRfcommChannel`
  Purpose: SDP discovery result DTO.
  Status: `privatize-candidate`
  Why: very small surface, effectively tied to RFCOMM discovery flow only.

### 2. Xiaomi protocol / auth / session

Module: `XiaomiProtocol`

- `EncodeXiaomiMessage`, `ParseXiaomiMessage`, `DecodeXiaomiMessages`, `BuildXiaomiNoiseProbeCommands`
  Purpose: Xiaomi packet codec and probe command generation.
  Status: `active`

- `XiaomiMessageType`, `XiaomiOpcode`, `XiaomiMessage`
  Purpose: shared protocol vocabulary.
  Status: `active`

- `XiaomiProbeCommand`
  Purpose: DTO for noise probe command list.
  Status: `single-caller`
  Why: narrow scope; mostly probe/debug path.

Module: `XiaomiAuth`

- `ComputeXiaomiChallengeResponse`, `GenerateRandomChallenge`
  Purpose: auth handshake cryptographic primitives.
  Status: `active`

Module: `XiaomiHandshake`

- `RunXiaomiAuthHandshake`
  Purpose: full Xiaomi auth handshake orchestration.
  Status: `single-caller`
  Used from: `XiaomiControlConnection`

Module: `XiaomiControlSession`

- `AppendAndDecodeXiaomiMessages`, `FormatXiaomiChunkLine`, `FormatXiaomiMessageLine`, `SendXiaomiInfoRequests`, `IsXiaomiReportStatusNotification`, `SendXiaomiReportStatusAck`
  Purpose: common Xiaomi session traffic parsing/printing/ack logic.
  Status: `active`

### 3. Xiaomi battery / mode / cache pipeline

Module: `XiaomiBatteryCodec`

- `ParseXiaomiBatteryRaw`, `ExtractBatterySnapshotFromXiaomiPayload`, `ExtractPreferredXiaomiBatterySnapshot`, `MergeXiaomiSnapshots`, `BuildXiaomiBatteryReadings`, `HasAnyBattery`
  Purpose: vendor battery payload decoding.
  Status: `active`

Module: `XiaomiBatteryReadings`

- `XiaomiResolvedTwsComponentCount`, `HasUsefulXiaomiTwsReadings`, `XiaomiReadingsRichnessScore`
  Purpose: Xiaomi TWS reading quality heuristics.
  Status: `active`

Module: `XiaomiClassicBatterySession`

- `RunXiaomiClassicBatterySession`
  Purpose: classic RFCOMM Xiaomi battery session.
  Status: `single-caller`
  Used from: `ClassicBluetoothBatteryFallback`

- `XiaomiClassicBatterySessionResult`
  Purpose: return DTO for classic session.
  Status: `privatize-candidate`
  Why: currently lives in header, but is only needed in one narrow call chain.

Module: `XiaomiNoiseModeCodec`

- `ParseXiaomiNoiseModeCode`, `ParseXiaomiNoiseSubmodeCodeFromF4Payload`, `XiaomiNoiseModeCodeToText`, `XiaomiNoiseSubmodeCodeToText`
  Purpose: noise-control mode/submode parsing and text mapping.
  Status: `active`

Module: `XiaomiModeCache`

- `PutXiaomiModeCacheEntry`, `TryGetXiaomiModeCacheEntry`, `TryGetXiaomiSubmodeCacheEntry`
  Purpose: runtime cache for last observed Xiaomi mode/submode.
  Status: `active`

Module: `XiaomiPersistentCache`

- `PutPersistentXiaomiSnapshot`, `GetPersistentXiaomiSnapshot`, `SnapshotFromBatteryReadings`
  Purpose: disk persistence for Xiaomi TWS fallback battery cache.
  Status: `active`

### 4. ZMI / HFP fallback path

Module: `ZmiBatteryCodec`

- `NormalizeZmiVendorBatteryScalar`, `ExtractZmiSerialPatternSnapshot`, `ExtractZmiSerialTextSnapshot`
  Purpose: ZMI-specific decode helpers.
  Status: `active`

Module: `ZmiSerialBatterySession`

- `TryReadZmiSerialBatteryFromSocket`
  Purpose: ZMI serial session reader.
  Status: `single-caller`
  Used from: `ClassicBluetoothBatteryFallback`

Module: `HfpBatterySession`

- `ReplyToHfpAgCommand`, `ParseAtBatteryPercentFromLine`
  Purpose: HFP/AT fallback parsing and replies.
  Status: `active`

Module: `ClassicBluetoothBatteryFallback`

- `TryReadGenericClassicHfpBattery`
  Purpose: generic classic HFP fallback percent read.
  Status: `single-caller`

- `TryReadXiaomiClassicBattery`
  Purpose: classic Xiaomi/ZMI battery fallback entry point.
  Status: `active`

### 5. PnP / vendor hints

Module: `BluetoothPnpUtils`

- `FindBthEnumInstanceIdsByAddress`, `FindBthLeInstanceIdsByAddress`, `ReadDevNodeUInt32Property`, `ReadDevNodeStringListProperty`
  Purpose: Windows PnP/devnode property access.
  Status: `active`

Module: `BluetoothPnpHints`

- `ReadBluetoothVisualHintsFromPnpAddress`, `ReadPhoneHfpBatteryHintFromPnpAddress`
  Purpose: visual hints and phone battery hint reads from PnP.
  Status: `single-caller`
  Notes: useful, but currently mostly consumed through query/support builders.

Module: `ZmiVendorBatteryHints`

- `AppendZmiVendorHintPropertyRequests`, `ReadZmiVendorBatteryHintFromPnpAddress`, `ReadZmiVendorBatteryHint`
  Purpose: vendor property battery hints for ZMI-family devices.
  Status: `active`

### 6. Discovery / aggregation / query orchestration

Module: `WindowsBatteryEntryUtils`

- `EndpointCandidate`
  Purpose: endpoint candidate DTO.
  Status: `active`

- `PopulateBluetoothVisualHintsFromEndpointCandidate`, `AppendBatteryEntriesFromReadings`, `AppendSingleBatteryEntry`, `TryAppendZmiVendorBatteryEntries`
  Purpose: DTO assembly and endpoint-entry shaping.
  Status: `active`

Module: `WindowsBatteryQueryReaders`

- `ReadConnectedBluetoothDeviceBatteryFast`, `ReadAssociationEndpointBattery`, `ReadGenericDeviceBattery`
  Purpose: Windows query readers for fast/AEP/generic battery sources.
  Status: `active`

Module: `WindowsBatteryAggregation`

- `DeviceBatteryAccumulator`, `CollectDisconnectedPairedBluetoothEntries`, `ApplyPnpVisualHints`, `FinalizeCollectedEntries`
  Purpose: merge/dedupe/finalize layer.
  Status: `active`

Module: `WindowsBleCandidateBatteryCollector`

- `CollectBleCandidateBatteryEntries`
  Purpose: BLE online device collection pipeline.
  Status: `single-caller`
  Used from: `WinRtBatteryProvider`

Module: `WindowsTwsCandidateBatteryCollector`

- `CollectTwsCandidateBatteryEntries`
  Purpose: TWS/AEP Xiaomi/ZMI fallback pipeline.
  Status: `single-caller`
  Used from: `WinRtBatteryProvider`

Module: `WindowsBluetoothTargetResolver`

- `ResolvedBluetoothTarget`, `DeviceBatteryEntryMatchesHint`, `CollectResolvedBluetoothTargets`
  Purpose: address resolution and hint matching for control/diagnostics targets.
  Status: `active`

### 7. Runtime caches and control actions

Module: `XiaomiBatteryCaches`

- `XiaomiClassicBatteryCache`, `XiaomiAdvertisementBatteryCache`
  Purpose: live + persistent Xiaomi battery cache orchestration.
  Status: `active`

- `XiaomiReadResult`
  Purpose: cache read result DTO.
  Status: `privatize-candidate`
  Why: only relevant inside cache flow; should probably not stay as header-level shared type forever.

Module: `XiaomiControlActions`

- `ProbeXiaomiNoiseControlForTarget`, `ObserveXiaomiControlSessionForTarget`, `ObserveZmiSerialSessionForTarget`, `SetXiaomiNoiseModeForTarget`, `SendXiaomiControlCandidateForTarget`, `SetXiaomiNoiseSubmodeForTarget`, `SetNoiseControlModeForAddress`
  Purpose: control/diagnostics action surface extracted from provider.
  Status: `single-caller`
  Used from: `WinRtBatteryProvider`
  Notes: this is good extraction, but it is still a provider-facing facade, not a widely reused service yet.

Module: `WindowsBatteryProviderSupport`

- `GetWindowsBatteryProviderRuntimeOptions`, `WindowsBatteryProviderDebugLog`, `EnsureWindowsBatteryProviderApartmentInitialized`, `MakeWindowsBatteryQueryReaderContext`, `MakeWindowsBleCandidateBatteryCollectorContext`, `MakeWindowsTwsCandidateBatteryCollectorContext`, `MakeWindowsXiaomiControlActionContext`, `TryOpenBleDeviceByAddress`, `ResolveConnectedXiaomiControlTarget`, `ResolveConnectedZmiControlTarget`, `ResolveAnyBluetoothTarget`
  Purpose: final support layer that removed env/debug/heuristic glue from `WinRtBatteryProvider`.
  Status: `single-caller`
  Used from: `WinRtBatteryProvider`

- `WindowsBatteryProviderRuntimeOptions`
  Purpose: runtime env/config DTO.
  Status: `privatize-candidate`
  Why: currently only meaningful to provider/support code.

## What Looks Actually Unnecessary

I did **not** find dead extracted functions with no callers.

What does look oversized or too public:

1. `DiscoveredRfcommChannel`
   Reason: narrow DTO, likely should be internal to transport/RFCOMM discovery layer.

2. `XiaomiClassicBatterySessionResult`
   Reason: single-caller result type; could become private to classic battery fallback layer.

3. `XiaomiReadResult`
   Reason: cache-internal DTO exposed via header.

4. `WindowsBatteryProviderRuntimeOptions`
   Reason: provider/support-only data shape, not really shared platform API.

5. `XiaomiProbeCommand`
   Reason: probe/debug-specific DTO with very narrow usage.

## Practical Conclusion

- The extraction was not pointless: the moved API is mostly live.
- The main remaining cleanup is **not** deleting whole extracted modules.
- The useful cleanup is narrowing header surface and moving several DTOs/types back to `.cpp` or making them module-private.

Best next cleanup pass for this area:

1. Privatize `WindowsBatteryProviderRuntimeOptions`.
2. Privatize `XiaomiReadResult`.
3. Privatize `XiaomiClassicBatterySessionResult`.
4. Consider hiding `DiscoveredRfcommChannel` behind transport implementation if no second consumer appears.
