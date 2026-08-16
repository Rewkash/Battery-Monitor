# Windows release process

## Product naming

- **Display name: `ChargeView`** — the human-readable name shown in UI titles, the installer
  wizard, shortcuts and release notes. Defined once as `BATTERY_MONITOR_DISPLAY_NAME` (with
  `BATTERY_MONITOR_PUBLISHER`) in `CMakeLists.txt`; the installer derives its display strings
  from these defines.
- **Internal identity: `BatteryMonitor` / `battery-monitor`** — the CMake project name, target
  and executable names (`battery-monitor.exe`, `battery-monitor-cli.exe`,
  `battery-monitor-maintenance.exe`), artifact file names
  (`battery-monitor-vX.Y.Z-win-x64.{msi,zip,bmup}`), the MSI UpgradeCode, registry keys under
  `Software\Orion Group\Battery Monitor`, and QSettings identifiers. This identity is frozen:
  renaming any of it breaks upgrades, auto-updates and settings migration.

## One-time GitHub setup

1. Configure repository secret `BATTERY_MONITOR_ED25519_PRIVATE_KEY_B64` with the 32-byte Ed25519 seed in Base64.
2. Protect the `release` environment with manual approval.
3. Protect `master` and tags matching `v*`.
4. Keep workflow permissions at read-only except `contents: write` in the release job.
5. Configure the required trusted Authenticode credentials. Release signing applies an RFC 3161 SHA-256 timestamp and verifies every PE signature:
   - `WINDOWS_SIGNING_PFX_BASE64`
   - `WINDOWS_SIGNING_PFX_PASSWORD`
   - optionally adapt signing for the certificate provider's timestamp URL/HSM.

Never commit PFX, PEM, private-key or password files. The private Ed25519 seed is consumed only from the environment. Pull-request workflows do not receive it.

Key rotation is an atomic release change: run `generate_release_secrets.py`, replace the public key in
`UpdateSecurity.cpp`, `sign_manifest.py`, and `verify_release.py`, verify all three values match
`ed25519-public.b64`, and only then store `ed25519-seed.b64` as the GitHub secret.

## Development signing

Self-signed signatures are only for development and do not establish public trust:

```powershell
.\scripts\signing\New-DevelopmentSigningCertificate.ps1
.\scripts\signing\Sign-DevelopmentBuild.ps1 `
  -BuildDirectory ".\build\Release" `
  -PfxPath "$env:TEMP\BatteryMonitorSigning\orion-group-development.pfx"
```

## Publishing

1. Update the only version source:

   ```cmake
   project(BatteryMonitor VERSION X.Y.Z LANGUAGES C CXX)
   ```

2. Build and test locally:

   ```powershell
   cmake --fresh -S . -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release
   ctest --test-dir build -C Release --output-on-failure --no-tests=ignore
   .\build\Release\battery-monitor-cli.exe --version
   ```

3. Commit and push the version change.
4. Create and push tag `vX.Y.Z` from the protected release branch.
5. Approve the `release` GitHub environment.
6. The workflow validates tag/CMake equality, builds GUI and CLI together, runs CTest, then a
   CLI smoke test that must print exactly the released version. The distributable is staged by
   a single `cmake --install` into `release-staging/app` (executables, the Qt runtime via
   windeployqt, `profiles/` and `resources/` — packaging scripts consume only this staging).
   From the staging it applies Authenticode when configured, builds and validates the per-user
   MSI, verifies the MSI product identity (ProductVersion equals the release version, UpgradeCode
   equals `BATTERY_MONITOR_MSI_UPGRADE_CODE`) via `scripts/release/Get-MsiInfo.ps1`, creates the
   portable `bmup-1` update bundle and ZIP, generates and signs the manifest, independently
   re-verifies everything (`verify_release.py`: manifest signature, bundle traversal with no
   duplicate paths and per-file hashes, bundle/MSI sizes and hashes, MSI identity, and version
   consistency across the tag, manifest and MSI), and smoke-tests the exact ZIP contents before
   publishing.
7. Re-download the GitHub Release assets and run `verify_release.py` with both `--bundle` and
   `--msi` (plus `--expected-version` and, when re-checking the MSI database,
   `--msi-info <json from Get-MsiInfo.ps1>`) before announcing the release.

For an explicitly self-signed test prerelease, configure `WINDOWS_TEST_SIGNING_PFX_BASE64` and
`WINDOWS_TEST_SIGNING_PFX_PASSWORD`, then tag `vX.Y.Z-test.N`. Test tags never fall back to the
production Authenticode credentials and are published as GitHub prereleases. Test signatures omit
network timestamping; production signatures continue to require RFC 3161 timestamps.

The manifest expires after 14 days. Ship a newer release before expiration. Lower sequence values than the last successfully started installation are rejected by clients. A protected metadata-refresh workflow is not implemented yet, so do not replace release metadata manually.
