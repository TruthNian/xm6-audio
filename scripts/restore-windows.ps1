# SPDX-License-Identifier: MIT
# Standalone recovery: uses only PowerShell and Windows' inbox A2DP driver.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidatePattern('^(?:[0-9A-Fa-f]{2}[:-]?){6}$')][string]$DeviceAddress,
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$address = ($DeviceAddress -replace '[:-]','').ToUpperInvariant()
# The inbox driver uses MEDIA; the native transport uses Bluetooth. Never
# constrain this lookup to a device class, or use a wildcard to rebind a radio.
$headset = @(Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -match '^BTHENUM\\\{0000110b-' -and
    $_.InstanceId -match "(?:_|&)$($address)_C[0-9A-F]+$"
})
if ($headset.Count -ne 1) { throw 'Exactly one present A2DP service must match the selected address.' }
if (-not $CheckOnly) {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run recovery as administrator.'
    }
}
$inboxInf = Join-Path $env:windir 'INF\microsoft_bluetooth_a2dp_src.inf'
if (-not (Test-Path -LiteralPath $inboxInf -PathType Leaf)) { throw 'Windows inbox A2DP INF is missing; repair Windows before deployment.' }
if (-not ('Xm6Audio.InboxRecovery' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
namespace Xm6Audio {
    [StructLayout(LayoutKind.Sequential)]
    struct DeviceInfo {
        public uint cbSize; public Guid ClassGuid; public uint DevInst; public UIntPtr Reserved;
    }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct InstallParams {
        public uint cbSize, Flags, FlagsEx;
        public IntPtr hwndParent, InstallMsgHandler, InstallMsgHandlerContext, FileQueue;
        public UIntPtr ClassInstallReserved; public uint Reserved;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=260)] public string DriverPath;
    }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct DriverInfo {
        public uint cbSize, DriverType; public UIntPtr Reserved;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=256)] public string Description;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=256)] public string MfgName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=256)] public string ProviderName;
        public System.Runtime.InteropServices.ComTypes.FILETIME DriverDate;
        public ulong DriverVersion;
    }
    public static class InboxRecovery {
        [DllImport("setupapi.dll", SetLastError=true)]
        static extern IntPtr SetupDiCreateDeviceInfoList(IntPtr guid, IntPtr parent);
        [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        static extern bool SetupDiOpenDeviceInfoW(IntPtr set, string id, IntPtr parent, uint flags, ref DeviceInfo dev);
        [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        static extern bool SetupDiGetDeviceInstallParamsW(IntPtr set, ref DeviceInfo dev, ref InstallParams p);
        [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        static extern bool SetupDiSetDeviceInstallParamsW(IntPtr set, ref DeviceInfo dev, ref InstallParams p);
        [DllImport("setupapi.dll", SetLastError=true)]
        static extern bool SetupDiBuildDriverInfoList(IntPtr set, ref DeviceInfo dev, uint type);
        [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        static extern bool SetupDiEnumDriverInfoW(IntPtr set, ref DeviceInfo dev, uint type, uint index, ref DriverInfo driver);
        [DllImport("setupapi.dll", SetLastError=true)]
        static extern bool SetupDiDestroyDriverInfoList(IntPtr set, ref DeviceInfo dev, uint type);
        [DllImport("setupapi.dll", SetLastError=true)]
        static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
        [DllImport("newdev.dll", ExactSpelling=true, SetLastError=true)]
        static extern bool DiInstallDevice(IntPtr parent, IntPtr set, ref DeviceInfo dev, ref DriverInfo driver, uint flags, out bool reboot);
        static void Check(bool ok) {
            if (!ok) throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        // Check-only builds a compatible list; it never installs a driver.
        public static bool Run(string instance, string inf, bool install) {
            IntPtr set = SetupDiCreateDeviceInfoList(IntPtr.Zero, IntPtr.Zero);
            if (set == new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
            var dev = new DeviceInfo(); dev.cbSize = (uint)Marshal.SizeOf(typeof(DeviceInfo));
            bool built = false;
            try {
                Check(SetupDiOpenDeviceInfoW(set, instance, IntPtr.Zero, 0, ref dev));
                var p = new InstallParams(); p.cbSize = (uint)Marshal.SizeOf(typeof(InstallParams));
                Check(SetupDiGetDeviceInstallParamsW(set, ref dev, ref p));
                p.Flags |= 0x00010000; // DI_ENUMSINGLEINF: only the named inbox INF
                p.FlagsEx |= 0x00000800; // DI_FLAGSEX_ALLOWEXCLUDEDDRVS
                p.DriverPath = inf;
                Check(SetupDiSetDeviceInstallParamsW(set, ref dev, ref p));
                Check(SetupDiBuildDriverInfoList(set, ref dev, 2)); built = true;
                var driver = new DriverInfo(); driver.cbSize = (uint)Marshal.SizeOf(typeof(DriverInfo));
                Check(SetupDiEnumDriverInfoW(set, ref dev, 2, 0, ref driver));
                if (!String.Equals(driver.ProviderName, "Microsoft", StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException("Refusing a non-Microsoft recovery driver.");
                bool reboot = false;
                if (install) Check(DiInstallDevice(IntPtr.Zero, set, ref dev, ref driver, 0, out reboot));
                return reboot;
            } finally {
                if (built) SetupDiDestroyDriverInfoList(set, ref dev, 2);
                SetupDiDestroyDeviceInfoList(set);
            }
        }
    }
}
'@
}
$reboot = [Xm6Audio.InboxRecovery]::Run($headset[0].InstanceId, $inboxInf, -not $CheckOnly)
if ($CheckOnly) {
    Write-Host 'Microsoft inbox A2DP recovery is available. No device binding was changed.'
} else {
    $service = (Get-PnpDeviceProperty -InstanceId $headset[0].InstanceId -KeyName DEVPKEY_Device_Service).Data
    if ($service -ne 'BthA2dp' -and -not $reboot) { throw 'Recovery did not select BthA2dp; inspect the device installation log.' }
    Write-Host "Microsoft inbox A2DP selected. Reboot required: $reboot"
}
