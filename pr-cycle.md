# Refactor PR Cycle

Date: 2026-03-19

## Rules

- One PR = one architectural topic.
- No new feature work mixed into refactor PRs.
- Keep JSON output backward compatible.
- Build must stay green after every step.
- Windows-specific code stays under `src/platform/windows`.
- Linux-specific code stays under `src/platform/linux`.

## Verification Gate For Every PR

1. `cmake --build build --config Debug --target battery-monitor-cli`
2. `build\\Debug\\battery-monitor-cli.exe --json`
3. If Qt build is enabled, GUI starts without immediate crash.
4. If a PR touches Windows battery logic, run at least one live-device smoke check.

## Status Board

### Done

#### PR-01. Thin app entry layer

- command parsing moved to `src/app/CommandLineOptions.*`
- output formatting moved to `src/app/BatteryOutputFormatter.*`
- platform command dispatch moved out of `main.cpp`

#### PR-02. Partial capability leak cleanup

- UI no longer uses `dynamic_cast<INoiseControlProvider*>`
- provider exposes noise-control access through a transitional contract hook

#### PR-03. Canonical noise-control vocabulary

- shared vocabulary layer added
- UI and Windows provider now speak the same mode/submode tokens

#### PR-04. Windows extraction wave 1

Extracted from `WinRtBatteryProvider.cpp`:

- Xiaomi protocol/auth/handshake/control helpers
- socket helpers
- Xiaomi battery codec and mode cache
- ZMI serial battery session
- HFP battery session
- persistent Xiaomi cache

#### PR-05. Windows extraction wave 2

Extracted from `WinRtBatteryProvider.cpp`:

- classic RFCOMM fallback
- Bluetooth PnP helpers
- phone PnP battery hints
- visual hint readers
- ZMI vendor hint readers

#### PR-06. Final `WinRtBatteryProvider` decomposition

Goal:

- leave only orchestration and provider contract glue in `WinRtBatteryProvider`

Status:

- env/config, heuristics, target-resolution and BLE-open helpers moved to `WindowsBatteryProviderSupport.*`
- file is now down to `257` lines
- public methods are now thin dispatch/orchestration only

### In Progress

#### PR-08. `BatteryWindow` state split

Goal:

- move settings, hidden-device state, ordering and runtime state out of the UI god-class

Status:

- settings persistence moved to `src/ui/BatteryWindowSettings.*`
- drag/drop row widget moved to `src/ui/DraggableDeviceRow.*`

### Todo

#### PR-07. Replace transitional capability hook with real core contract

Goal:

- stop exposing `GetNoiseControlProvider()` as the long-term solution
- introduce capability-aware app/service contract in `include/core`

#### PR-09. `BatteryWindow` rendering split

Goal:

- separate device-card rendering and interaction logic from window orchestration

#### PR-10. Shared analytics layer

Goal:

- unify history/stats/runtime label and analysis logic

#### PR-11. Linux D-Bus cleanup

Goal:

- typed property helpers
- explicit timeout policy
- less repeated BlueZ plumbing

#### PR-12. CMake target split

Goal:

- split the current broad static library into real architectural targets

## Recommended Next PR

The next safest concrete PR is:

`PR-08`: continue splitting `BatteryWindow` state and notification/runtime helpers.

Why:

- `WinRtBatteryProvider` is no longer the main blocker
- `BatteryWindow` is now the dominant god-class by size and responsibility
- settings extraction is already done, so alerts/runtime state is the next clean seam
