# SPDX-License-Identifier: MIT
# Read-only validation. Never installs certificates or changes boot policy.
[CmdletBinding(DefaultParameterSetName='Package')]
param(
    [Parameter(Mandatory=$true,ParameterSetName='Package')][string]$PackageDirectory,
    [Parameter(Mandatory=$true,ParameterSetName='Transport')][string]$TransportDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" |
    Sort-Object FullName | Select-Object -Last 1
if (-not $signtool) { throw 'Windows SDK SignTool is required for kernel-policy verification.' }
function Test-DriverCatalog([string]$Directory,[string]$Driver) {
    $cat = Join-Path $Directory "$Driver.cat"
    $signature = Get-AuthenticodeSignature -LiteralPath $cat
    if ($signature.Status -ne 'Valid' -or
        $signature.SignerCertificate.Subject -notmatch 'CN=Microsoft Windows( Hardware Compatibility Publisher)?[,\s]') {
        throw "No valid Microsoft driver catalog for $Driver. Windows audio has not been changed."
    }
    & $signtool.FullName verify /v /kp /c $cat (Join-Path $Directory "$Driver.sys")
    if ($LASTEXITCODE -ne 0) { throw "Kernel policy/catalog membership failed for $Driver.sys." }
    $members = @("$Driver.inf")
    if ($Driver -eq 'oa2dp') { $members += @('oa2dpsvc.exe','oa2dpctl.exe','ldac.dll','sbc.dll') }
    foreach ($member in $members) {
        & $signtool.FullName verify /v /pa /c $cat (Join-Path $Directory $member)
        if ($LASTEXITCODE -ne 0) { throw "Catalog membership failed for $member." }
    }
}
if ($PSCmdlet.ParameterSetName -eq 'Transport') {
    Test-DriverCatalog (Resolve-Path -LiteralPath $TransportDirectory).Path 'oa2dp'
} else {
    $package = (Resolve-Path -LiteralPath $PackageDirectory).Path
    foreach ($driver in @('oa2dp','oa2dpaudio')) {
        Test-DriverCatalog (Join-Path $package "drivers\$driver") $driver
    }
}
Write-Host 'Microsoft catalog validation passed for the checked payload. Hardware operation remains unverified.'
