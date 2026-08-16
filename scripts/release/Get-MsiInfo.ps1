# Extracts the Windows Installer product identity from an MSI database, verifies
# it against the expected release identity, and emits the identity as an object
# so callers can persist it as JSON for verify_release.py (--msi-info).
#
# Usage:
#   .\scripts\release\Get-MsiInfo.ps1 -MsiPath <msi> [-ExpectedVersion X.Y.Z]
#       [-ExpectedUpgradeCode {GUID-from-CMakeLists}]
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$MsiPath,

    # Release version the MSI ProductVersion must carry (numeric X.Y.Z).
    [string]$ExpectedVersion,

    # Expected UpgradeCode (braces optional), normally read from
    # BATTERY_MONITOR_MSI_UPGRADE_CODE in CMakeLists.txt.
    [string]$ExpectedUpgradeCode
)

$ErrorActionPreference = 'Stop'

$guidPattern = '^\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}$'
$versionPattern = '^\d+\.\d+\.\d+$'

# MSI SQL quotes identifiers with backticks; a double backtick is a literal
# backtick inside a double-quoted PowerShell string, and $quote is a SQL quote.
$quote = [char]39

function Get-MsiProperty {
    param(
        [Parameter(Mandatory = $true)] $Database,
        [Parameter(Mandatory = $true)] [string]$Name
    )
    $query = "SELECT ``Value`` FROM ``Property`` WHERE ``Property`` = $quote$Name$quote"
    $view = $Database.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $Database, @($query))
    try {
        [void]$view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null)
        $record = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)
        if ($null -eq $record) {
            throw "MSI property '$Name' is missing"
        }
        return [string]$record.GetType().InvokeMember('StringData', 'GetProperty', $null, $record, @(1))
    }
    finally {
        [void]$view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view, $null)
    }
}

$msi = (Resolve-Path -LiteralPath $MsiPath).Path
$installer = New-Object -ComObject WindowsInstaller.Installer
$database = $null
try {
    # 0 = msiOpenDatabaseModeReadOnly
    $database = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer, @($msi, 0))
    $productName = Get-MsiProperty -Database $database -Name 'ProductName'
    $productVersion = Get-MsiProperty -Database $database -Name 'ProductVersion'
    $productCode = Get-MsiProperty -Database $database -Name 'ProductCode'
    $upgradeCode = Get-MsiProperty -Database $database -Name 'UpgradeCode'
    $manufacturer = Get-MsiProperty -Database $database -Name 'Manufacturer'
}
finally {
    foreach ($com in @($database, $installer)) {
        if ($null -ne $com) {
            [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($com)
        }
    }
}

if ($productVersion -notmatch $versionPattern) {
    throw "MSI ProductVersion '$productVersion' is not numeric X.Y.Z"
}
foreach ($pair in @(@('ProductCode', $productCode), @('UpgradeCode', $upgradeCode))) {
    if ($pair[1] -notmatch $guidPattern) {
        throw "MSI $($pair[0]) '$($pair[1])' is not a GUID"
    }
}
if ($ExpectedVersion -and $productVersion -ne $ExpectedVersion) {
    throw "MSI ProductVersion '$productVersion' does not match expected '$ExpectedVersion'"
}
if ($ExpectedUpgradeCode) {
    $expected = $ExpectedUpgradeCode.Trim().ToUpperInvariant()
    if (-not $expected.StartsWith('{')) { $expected = '{' + $expected + '}' }
    if ($upgradeCode.ToUpperInvariant() -ne $expected) {
        throw "MSI UpgradeCode '$upgradeCode' does not match expected '$expected'"
    }
}

[pscustomobject]@{
    productName    = $productName
    productVersion = $productVersion
    productCode    = $productCode.ToUpperInvariant()
    upgradeCode    = $upgradeCode.ToUpperInvariant()
    manufacturer   = $manufacturer
}
