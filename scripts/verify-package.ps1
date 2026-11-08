# SPDX-License-Identifier: MIT
# Read-only gate for a returned Microsoft-signed package. No trust-store edits.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PackageDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" |
    Sort-Object FullName | Select-Object -Last 1
if (-not $signtool) { throw 'Windows SDK SignTool is required for kernel-policy verification.' }
foreach ($driver in @('oa2dp','oa2dpaudio')) {
    $dir = Join-Path $package "drivers\$driver"
    $cat = Join-Path $dir "$driver.cat"
    $signature = Get-AuthenticodeSignature -LiteralPath $cat
    if ($signature.Status -ne 'Valid' -or
        $signature.SignerCertificate.Subject -notmatch 'CN=Microsoft Windows( Hardware Compatibility Publisher)?[,\s]') {
        throw "No valid Microsoft driver catalog for $driver. Windows audio has not been changed."
    }
    foreach ($file in @("$driver.sys", "$driver.inf")) {
        & $signtool.FullName verify /v /kp /c $cat (Join-Path $dir $file)
        if ($LASTEXITCODE -ne 0) { throw "Kernel policy/catalog membership failed for $file." }
    }
}
Write-Host 'Both packages pass Microsoft catalog and kernel-policy verification. Hardware operation is still unverified.'
