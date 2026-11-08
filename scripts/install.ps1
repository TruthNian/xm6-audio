# SPDX-License-Identifier: MIT
# Deployment for a separately Microsoft-signed native package. No signing bypass.
[CmdletBinding()]
param(
    [string]$PackageDirectory,
    [Parameter(Mandatory=$true)][ValidatePattern('^(?:[0-9A-Fa-f]{2}[:-]?){6}$')][string]$DeviceAddress,
    [switch]$CheckOnly,
    [switch]$RestoreWindows
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$address = ($DeviceAddress -replace '[:-]','').ToUpperInvariant()
$install = Join-Path $env:ProgramFiles 'XM6Audio'
$ctl = Join-Path $install 'oa2dpctl.exe'
function Invoke-Checked([string]$File, [string[]]$Arguments) {
    & $File @Arguments
    if ($LASTEXITCODE -notin @(0,3010)) { throw "$File failed ($LASTEXITCODE)." }
}
if ($RestoreWindows) {
    if (-not (Test-Path -LiteralPath $ctl)) { throw 'Installed recovery tool not found.' }
    Invoke-Checked $ctl @('restore',$address)
    if (Get-Service oa2dpsvc -ErrorAction SilentlyContinue) {
        Stop-Service oa2dpsvc -ErrorAction Stop
        Invoke-Checked sc.exe @('config','oa2dpsvc','start=','demand')
    }
    Write-Host 'Selected headset restored to the Windows driver. Pairing and radio driver were retained.'
    return
}
if (-not $PackageDirectory) { throw 'PackageDirectory is required.' }
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
& "$PSScriptRoot\verify-package.ps1" -PackageDirectory $package
foreach ($file in @('oa2dpsvc.exe','oa2dpctl.exe','ldac.dll','sbc.dll')) {
    if (-not (Test-Path -LiteralPath "$package\user-mode\$file")) { throw "Missing $file." }
}
if ($CheckOnly) { return }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run deployment as administrator after checking the package.'
}
if (Get-Service oa2dpsvc -ErrorAction SilentlyContinue) {
    throw 'A native service is already installed. Restore Windows before changing its package.'
}
if (Test-Path -LiteralPath $install) {
    throw 'The target installation directory already exists; keep the previous recovery files.'
}
$headset = @(Get-PnpDevice -Class Bluetooth | Where-Object {
    $_.InstanceId -match '^BTHENUM\\\{0000110b-' -and
    $_.InstanceId -match "_$($address)_C" -and $_.FriendlyName -match 'WH-1000XM6'
})
if ($headset.Count -ne 1) { throw 'Exactly one paired XM6 A2DP service must match the selected address.' }
$stateDir = Join-Path $env:ProgramData 'XM6Audio'
New-Item -ItemType Directory -Path $stateDir -Force | Out-Null
[ordered]@{
    address = $address
    instance = $headset[0].InstanceId
    previousInf = (Get-PnpDeviceProperty -InstanceId $headset[0].InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data
} | ConvertTo-Json | Set-Content -LiteralPath "$stateDir\install.local.json" -Encoding UTF8
New-Item -ItemType Directory -Path $install | Out-Null
foreach ($file in @('oa2dpsvc.exe','oa2dpctl.exe','ldac.dll','sbc.dll')) {
    Copy-Item -LiteralPath "$package\user-mode\$file" -Destination $install
}
$bindingAttempted = $false
try {
    foreach ($driver in @('oa2dpaudio','oa2dp')) {
        Invoke-Checked pnputil.exe @('/add-driver',"$package\drivers\$driver\$driver.inf")
    }
    Invoke-Checked $ctl @('set',$address,'codecs=ldac,aac,sbc','sample_rate=0','bits=0',
        'channel_mode=stereo','ldac_quality=hq','adaptive_bitrate=false','follow_source_rate=true')
    New-Service -Name oa2dpsvc -BinaryPathName ('"' + (Join-Path $install 'oa2dpsvc.exe') + '"') `
        -StartupType Automatic -DisplayName 'XM6 Audio Native Codec Service' `
        -Description 'Encodes the native XM6 audio endpoint through the Windows Bluetooth stack.' | Out-Null
    $bindingAttempted = $true
    Invoke-Checked $ctl @('use',$address)
    Start-Service oa2dpsvc
} catch {
    $failure = $_
    if ($bindingAttempted) {
        & $ctl restore $address
        if ($LASTEXITCODE -ne 0) { Write-Warning 'Automatic device restore failed; use the saved instance and Microsoft A2DP driver.' }
    }
    if (Get-Service oa2dpsvc -ErrorAction SilentlyContinue) {
        Stop-Service oa2dpsvc -ErrorAction SilentlyContinue
        & sc.exe delete oa2dpsvc | Out-Null
    }
    throw $failure
}
Write-Host 'Native package installed. Select XM6 in Windows Sound settings; actual sound, volume, controls and reconnection still require hardware acceptance.'
