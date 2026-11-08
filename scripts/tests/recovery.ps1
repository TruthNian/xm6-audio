# SPDX-License-Identifier: MIT
# Exercise the real recovery entry points with fake PnP / installation APIs.
# Run in a fresh PowerShell process; no device or certificate is changed.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$scripts = Split-Path $PSScriptRoot -Parent
Add-Type -AssemblyName System.ServiceProcess
Add-Type @'
using System;
using System.Collections.Generic;
namespace Xm6Audio {
    public static class InboxRecovery {
        public static int Calls, Installs;
        public static string Instance;
        public static bool ThrowOnInstall, RebootRequired;
        public static List<string> Timeline = new List<string>();
        public static bool Run(string instance, string inf, bool install) {
            Calls++; Instance = instance;
            Timeline.Add(install ? "bind" : "preflight");
            if (install) {
                Installs++;
                if (ThrowOnInstall) throw new InvalidOperationException("Simulated binding failure");
            }
            return install && RebootRequired;
        }
    }
    public class FakeCodecService {
        public string Name { get { return "oa2dpsvc"; } }
        public string Status = "Running";
        public int Stops, Starts;
        public bool FailStop, FailStopWait, FailStart;
        public void Stop() {
            InboxRecovery.Timeline.Add("stop"); Stops++;
            if (FailStop) throw new InvalidOperationException("Simulated stop failure");
            Status = "Stopped";
        }
        public void WaitForStatus(object wanted, TimeSpan timeout) {
            string state = wanted.ToString();
            InboxRecovery.Timeline.Add("wait-" + state);
            if (FailStopWait && state == "Stopped") throw new TimeoutException("Simulated stop timeout");
            if (Status == "StopPending" && state == "Stopped") Status = "Stopped";
            if (Status != state) throw new InvalidOperationException("Unexpected service state");
        }
        public void Refresh() { }
        public void Start() {
            InboxRecovery.Timeline.Add("start"); Starts++;
            if (FailStart) throw new InvalidOperationException("Simulated start failure");
            Status = "Running";
        }
    }
}
'@
$global:Xm6RecoveryTestDevices = @()
$global:Xm6RecoveryInboxAvailable = $true
$global:Xm6RecoveryCodecService = $null
$global:Xm6RecoveryServiceReads = 0
$global:Xm6RecoveryServiceQueryFailure = $false
function Get-Service {
    [CmdletBinding()] param()
    $global:Xm6RecoveryServiceReads++
    if ($global:Xm6RecoveryServiceQueryFailure) { throw 'Simulated SCM query failure' }
    if ($global:Xm6RecoveryCodecService) { $global:Xm6RecoveryCodecService }
}
function Get-PnpDeviceProperty {
    [CmdletBinding()] param([string]$InstanceId, [string]$KeyName)
    if ($InstanceId -ne [Xm6Audio.InboxRecovery]::Instance -or $KeyName -ne 'DEVPKEY_Device_Service') {
        throw 'Unexpected device verification'
    }
    [Xm6Audio.InboxRecovery]::Timeline.Add('verify')
    [pscustomobject]@{Data='BthA2dp'}
}
function Test-Path {
    [CmdletBinding()] param([string]$LiteralPath, [string]$PathType)
    if ($LiteralPath -eq (Join-Path $env:windir 'INF\microsoft_bluetooth_a2dp_src.inf')) {
        return $global:Xm6RecoveryInboxAvailable
    }
    Microsoft.PowerShell.Management\Test-Path @PSBoundParameters
}
function Get-PnpDevice {
    [CmdletBinding()] param([switch]$PresentOnly)
    if (-not $PresentOnly) { throw 'Recovery must select present devices.' }
    $global:Xm6RecoveryTestDevices
}
function Get-ItemPropertyValue { throw 'Recovery must not read the native service registration.' }
function Assert-Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Assert-Rejected([scriptblock]$Action) {
    $before = [Xm6Audio.InboxRecovery]::Calls
    $rejected = $false
    try { & $Action } catch { $rejected = $true }
    Assert-Check $rejected 'Ambiguous or absent target was not rejected.'
    Assert-Check ([Xm6Audio.InboxRecovery]::Calls -eq $before) 'Invalid selection reached the native API.'
}
$address = '001122334455'
$id = 'BTHENUM\{0000110B-0000-1000-8000-00805F9B34FB}_VID&0002054C\7&123&0&001122334455_C00000000'
$media = [pscustomobject]@{ Class='MEDIA'; FriendlyName='WH-1000XM6 Stereo'; InstanceId=$id }
$native = [pscustomobject]@{ Class='Bluetooth'; FriendlyName='XM6 Audio'; InstanceId=$id }
foreach ($device in @($media,$native)) {
    $global:Xm6RecoveryTestDevices = @($device)
    # No package argument; no service registration; no verifier invocation.
    & "$scripts\install.ps1" -DeviceAddress $address -RestoreWindows -CheckOnly
    Assert-Check ([Xm6Audio.InboxRecovery]::Instance -eq $id) 'Wrong A2DP service selected.'
}
$global:Xm6RecoveryTestDevices = @($media,$native)
Assert-Rejected { & "$scripts\restore-windows.ps1" -DeviceAddress $address -CheckOnly }
$global:Xm6RecoveryTestDevices = @([pscustomobject]@{
    InstanceId=$id.Replace('001122334455','001122334456'); Class='MEDIA'
})
Assert-Rejected { & "$scripts\restore-windows.ps1" -DeviceAddress $address -CheckOnly }
$global:Xm6RecoveryTestDevices = @([pscustomobject]@{
    InstanceId=$id.Replace('0000110B','0000111E'); Class='Bluetooth'
})
Assert-Rejected { & "$scripts\restore-windows.ps1" -DeviceAddress $address -CheckOnly }
$global:Xm6RecoveryTestDevices = @($media)
$global:Xm6RecoveryInboxAvailable = $false
Assert-Rejected { & "$scripts\restore-windows.ps1" -DeviceAddress $address -CheckOnly }
Assert-Check ([Xm6Audio.InboxRecovery]::Calls -eq 2) 'Unexpected native API calls.'
Assert-Check ([Xm6Audio.InboxRecovery]::Installs -eq 0) 'Check-only attempted to install a driver.'
Assert-Check ($global:Xm6RecoveryServiceReads -eq 0) 'Check-only inspected or changed the codec service.'

# Every mutating API above is fake. The actual entry point still enforces its
# administrator requirement, so these control-flow checks run only when elevated.
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    function Reset-Scenario($CodecService) {
        [Xm6Audio.InboxRecovery]::Calls = 0
        [Xm6Audio.InboxRecovery]::Installs = 0
        [Xm6Audio.InboxRecovery]::ThrowOnInstall = $false
        [Xm6Audio.InboxRecovery]::RebootRequired = $false
        [Xm6Audio.InboxRecovery]::Timeline.Clear()
        $global:Xm6RecoveryInboxAvailable = $true
        $global:Xm6RecoveryCodecService = $CodecService
        $global:Xm6RecoveryServiceReads = 0
        $global:Xm6RecoveryServiceQueryFailure = $false
        $global:Xm6RecoveryTestDevices = @($native)
    }
    function Assert-Order([string]$Expected) {
        Assert-Check (([Xm6Audio.InboxRecovery]::Timeline -join ',') -eq $Expected) 'Unsafe recovery operation order.'
    }
    function Invoke-FailingRestore {
        $rejected = $false
        try { & "$scripts\restore-windows.ps1" -DeviceAddress $address } catch { $rejected = $true }
        Assert-Check $rejected 'A simulated recovery failure was ignored.'
    }
    Reset-Scenario $null
    & "$scripts\restore-windows.ps1" -DeviceAddress $address
    Assert-Order 'preflight,bind,verify'

    $codec = New-Object Xm6Audio.FakeCodecService
    $codec.Status = 'Stopped'
    Reset-Scenario $codec
    & "$scripts\restore-windows.ps1" -DeviceAddress $address
    Assert-Order 'preflight,bind,verify'
    Assert-Check ($codec.Starts -eq 0 -and $codec.Stops -eq 0) 'A previously stopped service changed state.'

    $codec = New-Object Xm6Audio.FakeCodecService
    Reset-Scenario $codec
    # A second headset must not be rebound, and its shared service must resume.
    $global:Xm6RecoveryTestDevices += [pscustomobject]@{InstanceId=$id.Replace('001122334455','001122334456')}
    & "$scripts\restore-windows.ps1" -DeviceAddress $address
    Assert-Order 'preflight,stop,wait-Stopped,bind,verify,start,wait-Running'
    Assert-Check ($codec.Status -eq 'Running') 'Shared codec service was not resumed.'
    Assert-Check ([Xm6Audio.InboxRecovery]::Instance -eq $id) 'Recovery changed the other headset.'

    $codec = New-Object Xm6Audio.FakeCodecService
    $codec.FailStop = $true
    Reset-Scenario $codec
    Invoke-FailingRestore
    Assert-Order 'preflight,stop'
    Assert-Check ([Xm6Audio.InboxRecovery]::Installs -eq 0) 'Rebinding proceeded after failed service stop.'

    $codec = New-Object Xm6Audio.FakeCodecService
    $codec.FailStopWait = $true
    Reset-Scenario $codec
    Invoke-FailingRestore
    Assert-Order 'preflight,stop,wait-Stopped,start,wait-Running'
    Assert-Check ([Xm6Audio.InboxRecovery]::Installs -eq 0) 'Rebinding proceeded without a confirmed service stop.'

    $codec = New-Object Xm6Audio.FakeCodecService
    Reset-Scenario $codec
    [Xm6Audio.InboxRecovery]::ThrowOnInstall = $true
    Invoke-FailingRestore
    Assert-Order 'preflight,stop,wait-Stopped,bind,start,wait-Running'
    Assert-Check ($codec.Status -eq 'Running') 'Binding failure left the codec service stopped.'

    $codec = New-Object Xm6Audio.FakeCodecService
    $codec.FailStart = $true
    Reset-Scenario $codec
    $resumeWarnings = @()
    & "$scripts\restore-windows.ps1" -DeviceAddress $address -WarningVariable resumeWarnings
    Assert-Order 'preflight,stop,wait-Stopped,bind,verify,start'
    Assert-Check ([Xm6Audio.InboxRecovery]::Installs -eq 1 -and $resumeWarnings.Count -eq 1) 'A failed native service restart blocked or hid inbox recovery.'

    $codec = New-Object Xm6Audio.FakeCodecService
    Reset-Scenario $codec
    [Xm6Audio.InboxRecovery]::RebootRequired = $true
    & "$scripts\restore-windows.ps1" -DeviceAddress $address
    Assert-Order 'preflight,stop,wait-Stopped,bind,verify'
    Assert-Check ($codec.Starts -eq 0) 'A pending driver replacement was reopened.'

    $codec = New-Object Xm6Audio.FakeCodecService
    $codec.Status = 'StopPending'
    Reset-Scenario $codec
    & "$scripts\restore-windows.ps1" -DeviceAddress $address
    Assert-Order 'preflight,wait-Stopped,bind,verify'
    Assert-Check ($codec.Starts -eq 0 -and $codec.Stops -eq 0) 'An existing stop request was overridden.'

    Reset-Scenario (New-Object Xm6Audio.FakeCodecService)
    $global:Xm6RecoveryServiceQueryFailure = $true
    Invoke-FailingRestore
    Assert-Order 'preflight'
    Assert-Check ([Xm6Audio.InboxRecovery]::Installs -eq 0) 'SCM query failure was mistaken for service absence.'
    Write-Host 'Recovery lifecycle checks passed: service absence, stopped/running/pending states, stop-before-bind, failure restoration, pending reboot and SCM errors; all mutating APIs fake.'
} else {
    Write-Host 'Recovery lifecycle mock checks require an elevated test process; read-only checks passed.'
}
Remove-Variable Xm6RecoveryTestDevices -Scope Global
Remove-Variable Xm6RecoveryInboxAvailable -Scope Global
Remove-Variable Xm6RecoveryCodecService -Scope Global
Remove-Variable Xm6RecoveryServiceReads -Scope Global
Remove-Variable Xm6RecoveryServiceQueryFailure -Scope Global
Write-Host 'Recovery checks passed: MEDIA/Bluetooth selection, absent registration/package, ambiguous/wrong target, read-only behavior.'
