# Windows release process

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
   .\build\Release\battery-monitor-cli.exe --version
   ```

3. Commit and push the version change.
4. Create and push tag `vX.Y.Z` from the protected release branch.
5. Approve the `release` GitHub environment.
6. The workflow validates tag/CMake equality, builds GUI and CLI together, applies Authenticode when configured, creates one `bmup-1` bundle, hashes it, generates and signs the manifest, independently verifies all artifacts, then publishes them.
7. Re-download the three GitHub Release assets and run `verify_release.py` before announcing the release.

The manifest expires after 14 days. Ship a newer release before expiration. Lower sequence values than the last successfully started installation are rejected by clients. A protected metadata-refresh workflow is not implemented yet, so do not replace release metadata manually.
