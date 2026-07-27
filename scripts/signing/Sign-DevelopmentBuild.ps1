param(
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$PfxPath,
    [string]$Password = "battery-monitor-development-only",
    [string]$TimestampUrl = "http://timestamp.digicert.com",
    [switch]$SkipTimestamp
)

$ErrorActionPreference = "Stop"
$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe |
    Where-Object FullName -Match "\\x64\\signtool.exe$" |
    Sort-Object FullName -Descending |
    Select-Object -First 1
if (-not $signtool) { throw "signtool.exe not found" }
$files = Get-ChildItem $BuildDirectory -Recurse -Include *.exe,*.dll
foreach ($file in $files) {
    if ($SkipTimestamp) {
        & $signtool.FullName sign /fd SHA256 /f $PfxPath /p $Password $file.FullName
    } else {
        & $signtool.FullName sign /fd SHA256 /tr $TimestampUrl /td SHA256 /f $PfxPath /p $Password $file.FullName
    }
    if ($LASTEXITCODE -ne 0) { throw "Signing failed: $($file.FullName)" }
    & $signtool.FullName verify /pa $file.FullName
    if ($LASTEXITCODE -ne 0) { throw "Signature verification failed: $($file.FullName)" }
}
$files | ForEach-Object { Get-AuthenticodeSignature $_.FullName | Select-Object Path,Status,SignerCertificate }
