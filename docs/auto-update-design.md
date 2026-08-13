# Auto-update design

## Scope and decisions

The first implementation targets Windows x64. Linux remains supported by the application build but does not self-update yet. Releases and signed metadata are hosted as immutable assets in GitHub Releases at `Rewkash/Battery-Monitor`.

The updater is deliberately built from small project-native pieces instead of WinSparkle or Qt Installer Framework:

- WinSparkle is Windows-only, does not provide this project's required two-binary atomic transaction and crash rollback by itself, and would still require an installer integration.
- Qt Installer Framework is a separate installer ecosystem and maintenance application. It is appropriate for a future system-wide installer, but is disproportionately heavy for the current loose Qt deployment and does not by itself provide the required independent signed GitHub manifest policy.
- Qt Network is already part of the Qt runtime. A narrow custom client plus a separate maintenance tool gives explicit hash/signature checks and keeps GUI and CLI in one package.

Portable installations continue to use directory replacement. The per-user MSI installs under LocalAppData without elevation and is upgraded only through Windows Installer; the portable updater never replaces MSI-owned files.

## Trust and delivery

Release assets include:

- `battery-monitor-vX.Y.Z-win-x64.bmup`
- `battery-monitor-vX.Y.Z-win-x64.msi`
- `battery-monitor-vX.Y.Z-win-x64.zip`
- `update-manifest.json`
- `update-manifest.json.sig`

The exact UTF-8 bytes of `update-manifest.json` are signed with Ed25519. The public key and its SHA-256 key ID are compiled into the application. The private 32-byte seed exists only as the GitHub Actions secret `BATTERY_MONITOR_ED25519_PRIVATE_KEY_B64`.

The client verifies the signature **before parsing JSON**, validates expiration and a monotonic sequence, then verifies package size and SHA-256. HTTPS remains mandatory but is not the release trust anchor. Modifying assets on compromised hosting cannot produce a valid signature.

Manifest schema 1 retains the original `artifact` field for existing portable clients and may add a signed `msiArtifact`. New clients select the MSI only when the installer-owned HKCU marker matches the current installation path and the compiled-in UpgradeCode. Otherwise they select `bmup-1`.

Manifest signing protects release delivery. Authenticode separately identifies `Orion Group` to Windows and builds SmartScreen reputation. Development builds may use a self-signed certificate; production releases should use `WINDOWS_SIGNING_PFX_BASE64` and `WINDOWS_SIGNING_PFX_PASSWORD` until a managed signing service replaces PFX secrets.

## Package and transaction

`bmup-1` is an uncompressed deterministic container. Every entry contains a normalized UTF-8 relative path, size and SHA-256. Extraction rejects absolute/traversal/ADS/reserved/duplicate paths, truncation, oversized content and trailing data.

The running application downloads into user-local update storage, but extracts the verified package into a uniquely named sibling of the installation so directory renames stay on one volume. It copies and starts a dependency-free, statically linked `battery-monitor-maintenance.exe`, then exits normally. The maintenance process validates the sibling paths, waits for the old PID, renames the complete current directory to a unique backup, renames staging into place, and starts the same GUI or CLI executable. A one-use inherited event is signaled only after GUI initialization reaches the event loop or CLI provider construction succeeds. Missing health acknowledgement within 60 seconds causes the failed tree to be renamed aside and the previous tree to be restored.

The two public binaries and the complete Qt runtime are one signed bundle, so GUI and CLI cannot be updated to different versions. `project(BatteryMonitor VERSION ...)` remains the only version source and generates `BatteryMonitorVersion.h` for every target.

The MSI is a per-user WiX major-upgrade package installed under `%LOCALAPPDATA%\Programs\Orion Group\ChargeView`. Its UpgradeCode is stable across releases and each package receives a new ProductCode. The maintenance tool waits for the running application, invokes the system `msiexec.exe` with `/passive /norestart`, relaunches the installed binary, and commits the signed sequence after startup health acknowledgement. Windows Installer handles transaction rollback for installation failures; unlike portable updates, a successful MSI transaction is not automatically downgraded after a later startup-health failure.

## UX

The GUI checks asynchronously 5–30 seconds after startup. The tray menu provides a manual check and displays the current version. An available release opens a non-modal dialog with release notes, integrity information, progress, Later for optional releases, and Download and restart. Mandatory manifests hide Later. Verification failures never have an install-anyway action.

CLI supports:

```text
battery-monitor-cli --version
battery-monitor-cli --check-updates [--json]
```

## Security limitations

- A GitHub Actions/repository administrator or stolen signing secret can create an authorized malicious release. Move production Ed25519 signing to offline/KMS approval when release volume justifies it.
- A local administrator can replace binaries and update state.
- The bootstrap maintenance executable is copied out before directory replacement; self-update recovery beyond the retained rollback tree is not yet a stable-launcher design.
- Crash rollback observes explicit startup health only, not failures hours after launch.
- Mandatory policy currently has no grace deadline; it is a signed immediate policy flag.

## Deliberately deferred

- Linux updates: there is no existing Linux package format. Choose AppImage or native package ownership before implementing it.
- Delta updates: Qt deployment size and release frequency do not justify patch complexity yet. Full packages are simpler to authenticate and recover.
- Staged rollout: unnecessary at current scale and would require release promotion state and useful health telemetry.
- Stable/beta channels and key rotation/threshold roots: stable-only is sufficient initially; add an offline root role before multiple online signing keys exist.
- System-wide elevation and maintenance-tool self-update.
