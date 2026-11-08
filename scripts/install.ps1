# SPDX-License-Identifier: MIT
# Deploy one headset through its signed INF; Windows owns the codec service.
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
function Invoke-Checked([string]$File,[string[]]$Arguments) {
    & $File @Arguments
    if ($LASTEXITCODE -notin @(0,3010)) { throw "$File failed ($LASTEXITCODE)." }
}
function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run deployment or recovery as administrator.'
    }
}
function Get-InstalledTransport {
    $serviceKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\oa2dpsvc'
    $image = [Environment]::ExpandEnvironmentVariables((Get-ItemPropertyValue -LiteralPath $serviceKey -Name ImagePath))
    $image = $image.Trim('"')
    if ([IO.Path]::GetFileName($image) -ne 'oa2dpsvc.exe') { throw 'Unexpected registered codec service path.' }
    return [IO.Path]::GetDirectoryName($image)
}
function Get-NativeDevices {
    foreach ($device in (Get-PnpDevice -Class Bluetooth | Where-Object { $_.InstanceId -match '^BTHENUM\\\{0000110b-' })) {
        $service = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction Stop).Data
        if ($service -eq 'oa2dp') { $device }
    }
}
if ($RestoreWindows) {
    $transport = Get-InstalledTransport
    & "$PSScriptRoot\verify-package.ps1" -TransportDirectory $transport
    if ($CheckOnly) { return }
    Assert-Administrator
    Invoke-Checked (Join-Path $transport 'oa2dpctl.exe') @('restore',$address)
    if (@(Get-NativeDevices).Count -eq 0 -and (Get-Service oa2dpsvc).Status -ne 'Stopped') {
        Stop-Service oa2dpsvc
    }
    Write-Host 'Selected headset restored to Windows. Pairing and radio ownership were retained.'
    return
}
if (-not $PackageDirectory) { throw 'PackageDirectory is required.' }
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
& "$PSScriptRoot\verify-package.ps1" -PackageDirectory $package
$headset = @(Get-PnpDevice -Class Bluetooth | Where-Object {
    $_.InstanceId -match '^BTHENUM\\\{0000110b-' -and
    $_.InstanceId -match "_$($address)_C" -and $_.FriendlyName -match 'WH-1000XM6'
})
if ($headset.Count -ne 1) { throw 'Exactly one paired XM6 A2DP service must match the selected address.' }
$nativeArchitecture = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
if ([Environment]::OSVersion.Version.Build -lt 19041 -or $nativeArchitecture -ne 'AMD64') {
    throw 'This package targets Windows 10 2004 or later on x64.'
}
if (@(Get-NativeDevices | Where-Object { $_.InstanceId -ne $headset[0].InstanceId }).Count) {
    throw 'Another headset uses this shared native service. Coordinate its package update first.'
}
if ($CheckOnly) { return }
Assert-Administrator
if (-not ('Xm6Audio.NativeStore' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
namespace Xm6Audio {
    public static class NativeStore {
        [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool SetupGetInfDriverStoreLocation(string inf, IntPtr platform,
            string locale, StringBuilder buffer, uint capacity, out uint required);
        public static string Locate(string inf) {
            var buffer = new StringBuilder(32768); uint required;
            if (!SetupGetInfDriverStoreLocation(inf, IntPtr.Zero, null, buffer,
                    (uint)buffer.Capacity, out required))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            return System.IO.Path.GetDirectoryName(buffer.ToString());
        }
    }
}
'@
}
$stateDir = Join-Path $env:ProgramData 'XM6Audio'
New-Item -ItemType Directory -Path $stateDir -Force | Out-Null
[ordered]@{
    address = $address; instance = $headset[0].InstanceId
    previousInf = (Get-PnpDeviceProperty -InstanceId $headset[0].InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data
} | ConvertTo-Json | Set-Content -LiteralPath "$stateDir\install.local.json" -Encoding UTF8
$bindingAttempted = $false
$ctl = $null
try {
    foreach ($driver in @('oa2dpaudio','oa2dp')) {
        Invoke-Checked pnputil.exe @('/add-driver',"$package\drivers\$driver\$driver.inf")
    }
    # Execute the catalog-verified, protected Driver Store copy, not a loose EXE.
    $transport = [Xm6Audio.NativeStore]::Locate("$package\drivers\oa2dp\oa2dp.inf")
    & "$PSScriptRoot\verify-package.ps1" -TransportDirectory $transport
    $ctl = Join-Path $transport 'oa2dpctl.exe'
    Invoke-Checked $ctl @('set',$address,'codecs=ldac,aac,sbc','sample_rate=0','bits=0',
        'channel_mode=stereo','ldac_quality=hq','adaptive_bitrate=false','follow_source_rate=true')
    $bindingAttempted = $true
    $oldService = Get-Service oa2dpsvc -ErrorAction SilentlyContinue
    if ($oldService -and $oldService.Status -ne 'Stopped') { Stop-Service oa2dpsvc }
    Invoke-Checked $ctl @('use',$address)
    $service = Get-Service oa2dpsvc
    if ($service.Status -eq 'Stopped') { Start-Service oa2dpsvc }
    $service.WaitForStatus('Running',[TimeSpan]::FromSeconds(30))
} catch {
    $failure = $_
    if ($bindingAttempted -and $ctl) {
        & $ctl restore $address
        if ($LASTEXITCODE -ne 0) { Write-Warning 'Automatic restore failed; select the Microsoft A2DP driver for the saved device instance.' }
    }
    if (@(Get-NativeDevices).Count -eq 0 -and (Get-Service oa2dpsvc -ErrorAction SilentlyContinue)) {
        Stop-Service oa2dpsvc -ErrorAction SilentlyContinue
    }
    throw $failure
}
Write-Host 'Native driver package installed. Windows manages device-triggered codec service startup. Hardware acceptance is still required.'
