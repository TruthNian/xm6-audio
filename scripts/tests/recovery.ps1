# SPDX-License-Identifier: MIT
# Exercise the real recovery entry points with fake PnP / installation APIs.
# Run in a fresh PowerShell process; no device or certificate is changed.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$scripts = Split-Path $PSScriptRoot -Parent
Add-Type @'
namespace Xm6Audio {
    public static class InboxRecovery {
        public static int Calls, Installs;
        public static string Instance;
        public static bool Run(string instance, string inf, bool install) {
            Calls++; Instance = instance;
            if (install) Installs++;
            return false;
        }
    }
}
'@
$global:Xm6RecoveryTestDevices = @()
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
Assert-Check ([Xm6Audio.InboxRecovery]::Calls -eq 2) 'Unexpected native API calls.'
Assert-Check ([Xm6Audio.InboxRecovery]::Installs -eq 0) 'Check-only attempted to install a driver.'
Remove-Variable Xm6RecoveryTestDevices -Scope Global
Write-Host 'Recovery checks passed: MEDIA/Bluetooth selection, absent registration/package, ambiguous/wrong target, read-only behavior.'
