# SPDX-License-Identifier: MIT
# Read-only Windows state report. No pairing, radio, driver or service changes.
# Use Windows PowerShell 5.1 (powershell.exe), not PowerShell 7.
[CmdletBinding()]
param(
    [ValidateNotNullOrEmpty()][string]$DeviceName = 'WH-1000XM6',
    [switch]$IncludeOtherPaired
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -ne 'Desktop') {
    throw 'Run this script with Windows PowerShell 5.1 (powershell.exe).'
}
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Devices.Radios.Radio, Windows.System.Devices, ContentType=WindowsRuntime]
$null = [Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType=WindowsRuntime]
$null = [Windows.Devices.Bluetooth.BluetoothDevice, Windows.Devices.Bluetooth, ContentType=WindowsRuntime]
$asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.IsGenericMethodDefinition -and
    $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name.StartsWith('IAsyncOperation')
} | Select-Object -First 1
function Wait-WindowsResult($Operation, $ResultType) {
    $task = $asTask.MakeGenericMethod($ResultType).Invoke($null, @($Operation))
    if (-not $task.Wait(10000)) { throw 'Windows device query timed out.' }
    return ,$task.Result
}
function ConvertTo-PropertyMap($Properties) {
    $values = @{}
    if ($Properties -is [System.Collections.IDictionary]) {
        foreach ($key in $Properties.Keys) { $values[$key] = $Properties[$key] }
    } else {
        # WinRT exposes a COM IMapView in Windows PowerShell. String indexing
        # and .Keys do not reliably project it; enumerate IKeyValuePair entries.
        foreach ($entry in $Properties) {
            if ($entry.Key) { $values[$entry.Key] = $entry.Value }
        }
    }
    return $values
}
function Get-QueryFailure($Operation, $Failure) {
    $exception = $Failure.Exception.GetBaseException()
    # Exception messages can contain device IDs/registry paths. Keep errors
    # actionable without exposing identifiers that the report otherwise omits.
    return [pscustomobject]@{
        Operation = $Operation
        ErrorType = $exception.GetType().FullName
        HResult = ('0x{0:X8}' -f $exception.HResult)
    }
}
$report = [ordered]@{
    DeviceName = $DeviceName
    CapturedUtc = [DateTime]::UtcNow.ToString('o')
    BluetoothRadios = @()
    AssociationEndpoints = @()
    DeviceContainers = @()
    NativeAudio = [ordered]@{ ServiceState = $null; Version = $null; StatusLookup = 'Unavailable'; Sessions = @(); PresentEndpoints = @() }
    Errors = @()
}
$addresses = @()
$associationRead = $false
$targetFound = $false
try {
    $radios = Wait-WindowsResult ([Windows.Devices.Radios.Radio]::GetRadiosAsync()) `
        ([System.Collections.Generic.IReadOnlyList[Windows.Devices.Radios.Radio]])
    $report.BluetoothRadios = @($radios | Where-Object { [string]$_.Kind -eq 'Bluetooth' } |
        ForEach-Object { [pscustomobject]@{ State = [string]$_.State } })
} catch { $report.Errors += Get-QueryFailure 'Bluetooth radio query' $_ }
try {
    # Paired-device selector avoids requesting discovery of unpaired devices.
    $selector = [Windows.Devices.Bluetooth.BluetoothDevice]::GetDeviceSelectorFromPairingState($true)
    $properties = [string[]]@('System.Devices.Aep.IsConnected', 'System.Devices.Aep.IsPaired',
        'System.Devices.Aep.IsPresent', 'System.Devices.Aep.ProtocolId', 'System.Devices.Aep.DeviceAddress')
    $devices = Wait-WindowsResult ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync(
        $selector, $properties, [Windows.Devices.Enumeration.DeviceInformationKind]::AssociationEndpoint)) `
        ([Windows.Devices.Enumeration.DeviceInformationCollection])
    $report.AssociationEndpoints = @($devices | ForEach-Object {
        $target = [string]::Equals($_.Name, $DeviceName, [StringComparison]::OrdinalIgnoreCase)
        if ($target -or $IncludeOtherPaired) {
            $values = ConvertTo-PropertyMap $_.Properties
            if ($target) {
                $targetFound = $true
                $address = [string]$values['System.Devices.Aep.DeviceAddress'] -replace '[:-]', ''
                if ($address -match '^[0-9A-Fa-f]{12}$') { $addresses += $address }
            }
            [pscustomobject]@{
                Name = $(if ($target) { $_.Name } else { 'Other paired Bluetooth device' })
                Enabled = $_.IsEnabled
                PairingIsPaired = $_.Pairing.IsPaired
                Connected = $values['System.Devices.Aep.IsConnected']
                Paired = $values['System.Devices.Aep.IsPaired']
                Present = $values['System.Devices.Aep.IsPresent']
                Protocol = $values['System.Devices.Aep.ProtocolId']
            }
        }
    })
    $associationRead = $true
} catch { $report.Errors += Get-QueryFailure 'Bluetooth association query' $_ }
try {
    # Query local containers and compare names without constructing an AQS
    # expression from user input. No unrelated names/IDs appear in the report.
    $containers = Wait-WindowsResult ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync(
        '', [string[]]@('System.Devices.Connected'),
        [Windows.Devices.Enumeration.DeviceInformationKind]::DeviceContainer)) `
        ([Windows.Devices.Enumeration.DeviceInformationCollection])
    $report.DeviceContainers = @($containers | Where-Object {
        [string]::Equals($_.Name, $DeviceName, [StringComparison]::OrdinalIgnoreCase)
    } | ForEach-Object {
        $values = ConvertTo-PropertyMap $_.Properties
        [pscustomobject]@{ Name = $_.Name; Enabled = $_.IsEnabled; Connected = $values['System.Devices.Connected'] }
    })
} catch { $report.Errors += Get-QueryFailure 'Device container query' $_ }
try {
    $service = Get-CimInstance Win32_Service -Filter "Name='oa2dpsvc'"
    if ($service) {
        $report.NativeAudio.ServiceState = $service.State
        $image = $service.PathName.Trim('"')
        if (Test-Path -LiteralPath $image) {
            $report.NativeAudio.Version = (Get-Item -LiteralPath $image).VersionInfo.FileVersion
        }
    }
} catch { $report.Errors += Get-QueryFailure 'Native service query' $_ }
try {
    $sessions = @()
    foreach ($address in ($addresses | Select-Object -Unique)) {
        $key = 'HKLM:\SOFTWARE\OpenA2DP\Status\' + $address
        if (Test-Path -LiteralPath $key) {
            $sessions += Get-ItemProperty -LiteralPath $key | Select-Object State, Codec, SampleRate,
                Channels, Bits, InputContainerBits, InputValidBits, InputEncoding, Bitrate
        }
    }
    $report.NativeAudio.Sessions = $sessions
    $report.NativeAudio.StatusLookup = if ($addresses.Count) { 'Queried' }
        elseif ($associationRead -and -not $targetFound) { 'TargetNotFound' }
        else { 'DeviceAddressUnavailable' }
} catch { $report.Errors += Get-QueryFailure 'Native status query' $_ }
try {
    $report.NativeAudio.PresentEndpoints = @(Get-PnpDevice -PresentOnly -Class AudioEndpoint |
        Where-Object { $_.FriendlyName -and $_.FriendlyName.IndexOf($DeviceName, [StringComparison]::OrdinalIgnoreCase) -ge 0 } |
        Select-Object FriendlyName, Status)
} catch { $report.Errors += Get-QueryFailure 'Present audio endpoint query' $_ }
$report | ConvertTo-Json -Depth 6
