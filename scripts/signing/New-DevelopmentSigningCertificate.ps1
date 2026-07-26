param(
    [string]$OutputDirectory = "$env:TEMP\BatteryMonitorSigning",
    [string]$Password = "battery-monitor-development-only"
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$certificate = New-SelfSignedCertificate `
    -Subject "CN=Orion Group" `
    -Type CodeSigningCert `
    -CertStoreLocation "Cert:\CurrentUser\My" `
    -KeyAlgorithm RSA `
    -KeyLength 3072 `
    -HashAlgorithm SHA256 `
    -NotAfter (Get-Date).AddYears(2)
$securePassword = ConvertTo-SecureString -String $Password -AsPlainText -Force
$pfxPath = Join-Path $OutputDirectory "orion-group-development.pfx"
Export-PfxCertificate -Cert $certificate -FilePath $pfxPath -Password $securePassword | Out-Null
"Thumbprint=$($certificate.Thumbprint)"
"PfxPath=$pfxPath"
"Development-only certificate. It is not publicly trusted and must never be used for production releases."
