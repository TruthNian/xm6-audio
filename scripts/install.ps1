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
$script:RestartRequired = $false
function Invoke-Checked([string]$File,[string[]]$Arguments) {
    & $File @Arguments
    $result = $LASTEXITCODE
    if ($result -notin @(0,3010)) { throw "$File failed ($result)." }
    if ($result -eq 3010) { $script:RestartRequired = $true }
}
function Test-NativeDeviceState([string]$InstanceId) {
    $problem = [uint32](Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data
    $nodeStatus = [uint32](Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName DEVPKEY_Device_DevNodeStatus).Data
    if ($problem -eq 14 -or ($nodeStatus -band 0x100) -ne 0) {
        # CM_PROB_NEED_RESTART / DN_NEED_RESTART are pending activation,
        # including a previous operation whose installer returned success.
        $script:RestartRequired = $true
        return
    }
    if ($problem -ne 0 -or ($nodeStatus -band 0x8) -eq 0) {
        $problemStatus = (Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName DEVPKEY_Device_ProblemStatus).Data
        throw ('Native device is not started: problem {0}, status 0x{1:X8}.' -f $problem,[uint32]$problemStatus)
    }
}
function Update-NativeRenderDriver([string]$InfPath,[string]$InstanceId) {
    & pnputil.exe /add-driver $InfPath /install
    $result = $LASTEXITCODE
    if ($result -eq 3010) { $script:RestartRequired = $true; return }
    if ($result -eq 0) { return }
    if ($result -ne 259) { throw "Render driver update failed ($result)." }
    # PnPUtil also returns ERROR_NO_MORE_ITEMS when the matching device already
    # uses this package. Do not accept 259 for an absent or differently bound
    # child, or for other commands: establish the exact selected INF first.
    $child = Get-PnpDevice -InstanceId $InstanceId -PresentOnly -ErrorAction Stop
    if (-not $child) { throw 'Cannot verify the unchanged render device.' }
    $published = (Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName DEVPKEY_Device_DriverInfPath -ErrorAction Stop).Data
    if ($published -notmatch '^oem[0-9]+\.inf$') { throw 'Unexpected render driver INF.' }
    $selected = Join-Path $env:windir ('INF\'+$published)
    if ((Get-FileHash -LiteralPath $selected -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $InfPath -Algorithm SHA256).Hash) {
        throw 'The render device does not select the requested driver package.'
    }
    Test-NativeDeviceState $InstanceId
}
function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run deployment or recovery as administrator.'
    }
}
function Get-NativeDevices {
    foreach ($device in (Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match '^BTHENUM\\\{0000110b-' })) {
        $service = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction Stop).Data
        if ($service -eq 'oa2dp') { $device }
    }
}
if ($RestoreWindows) {
    # Recovery must work even if the service, native package or verifier is gone.
    & "$PSScriptRoot\restore-windows.ps1" -DeviceAddress $address -CheckOnly:$CheckOnly
    if ($CheckOnly) { return }
    if (@(Get-NativeDevices).Count -eq 0 -and (Get-Service oa2dpsvc -ErrorAction SilentlyContinue)) {
        Stop-Service oa2dpsvc -ErrorAction SilentlyContinue
    }
    Write-Host 'Selected headset restored to Windows. Pairing and radio ownership were retained.'
    return
}
if (-not $PackageDirectory) { throw 'PackageDirectory is required.' }
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
& "$PSScriptRoot\verify-package.ps1" -PackageDirectory $package
$headset = @(Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -match '^BTHENUM\\\{0000110b-' -and
    $_.InstanceId -match "(?:_|&)$($address)_C[0-9A-F]+$" -and $_.FriendlyName -match 'WH-1000XM6'
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
        static extern bool SetupCopyOEMInf(string source, string media, uint mediaType,
            uint flags, StringBuilder destination, uint capacity, out uint required, IntPtr component);
        [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool SetupGetInfDriverStoreLocation(string inf, IntPtr platform,
            string locale, StringBuilder buffer, uint capacity, out uint required);
        public static string Locate(string inf) {
            // The store lookup accepts a published OEM INF, not a loose source
            // path. Resolve the exact INF/catalog pair already staged above.
            var published = new StringBuilder(32768); uint required;
            if (!SetupCopyOEMInf(inf, null, 1, 8, published,
                    (uint)published.Capacity, out required, IntPtr.Zero)) {
                int error = Marshal.GetLastWin32Error();
                if (error != 80) throw new Win32Exception(error); // ERROR_FILE_EXISTS
            }
            if (published.Length == 0) throw new InvalidOperationException("No published driver INF returned.");
            var buffer = new StringBuilder(32768);
            if (!SetupGetInfDriverStoreLocation(published.ToString(), IntPtr.Zero, null, buffer,
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
    Test-NativeDeviceState $headset[0].InstanceId
    # Staging alone leaves an existing child bound to its previous package.
    # The preflight rejects another present native headset before this update.
    Update-NativeRenderDriver "$package\drivers\oa2dpaudio\oa2dpaudio.inf" ('OA2DP\Render\'+$address)
    Test-NativeDeviceState $headset[0].InstanceId
    $child = Get-PnpDevice -InstanceId ('OA2DP\Render\'+$address) -PresentOnly -ErrorAction SilentlyContinue
    if ($child) { Test-NativeDeviceState $child.InstanceId }
    if (-not $script:RestartRequired) {
        if (-not $child) { throw 'Native audio child did not enumerate.' }
        $service = Get-Service oa2dpsvc
        if ($service.Status -eq 'Stopped') { Start-Service oa2dpsvc }
        $service.WaitForStatus('Running',[TimeSpan]::FromSeconds(30))
    }
} catch {
    $failure = $_
    if ($bindingAttempted) {
        try {
            & "$PSScriptRoot\restore-windows.ps1" -DeviceAddress $address
        } catch {
            Write-Warning "Automatic restore failed: $_. Use restore-windows.ps1 or select the Microsoft A2DP driver for the saved device instance."
        }
    }
    if (@(Get-NativeDevices).Count -eq 0 -and (Get-Service oa2dpsvc -ErrorAction SilentlyContinue)) {
        Stop-Service oa2dpsvc -ErrorAction SilentlyContinue
    }
    throw $failure
}
if ($script:RestartRequired) {
    Write-Host 'Native package installed; a Windows restart is required to activate the update. Do not treat the staged drivers as running. Hardware acceptance remains pending.'
    exit 3010
}
Write-Host 'Native driver package installed without a requested restart. Windows manages device-triggered codec service startup. Hardware acceptance is still required.'
