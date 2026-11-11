# Execute the production interface AddReg sections in a disposable HKCU key.
# No device, driver, audio endpoint or machine configuration is changed.
$ErrorActionPreference = 'Stop'
$inf = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../driver/oa2dpaudio/oa2dpaudio.inf'))
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class EndpointInfTest {
 [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
 public static extern IntPtr SetupOpenInfFile(string file,IntPtr cls,uint style,out uint line);
 [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
 [return: MarshalAs(UnmanagedType.Bool)]
 public static extern bool SetupInstallFromInfSection(IntPtr hwnd,IntPtr inf,string section,uint flags,
     IntPtr root,string src,uint copy,IntPtr callback,IntPtr context,IntPtr set,IntPtr device);
 [DllImport("setupapi.dll")] public static extern void SetupCloseInfFile(IntPtr inf);
}
'@
$line = 0
$handle = [EndpointInfTest]::SetupOpenInfFile($inf,[IntPtr]::Zero,2,[ref]$line)
if($handle -eq [IntPtr](-1)){throw "Cannot open production INF, line $line, Win32=$([Runtime.InteropServices.Marshal]::GetLastWin32Error())"}
$relative = 'Software\XM6Audio\Tests\EndpointMetadata-' + [guid]::NewGuid().ToString('N')
$root = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($relative)
try {
    foreach($section in @('Oa2dpAudio.I.Wave','Oa2dpAudio.I.Topology')) {
        foreach($name in @($null,'WH-1000XM6','A different paired headset')) {
            $root.DeleteSubKeyTree('Case',$false)
            $case = $root.CreateSubKey('Case')
            try {
                if($null -ne $name){$case.SetValue('FriendlyName',$name,[Microsoft.Win32.RegistryValueKind]::String)}
                if(-not [EndpointInfTest]::SetupInstallFromInfSection([IntPtr]::Zero,$handle,$section,4,
                    $case.Handle.DangerousGetHandle(),$null,0,[IntPtr]::Zero,[IntPtr]::Zero,[IntPtr]::Zero,[IntPtr]::Zero)) {
                    throw "Production AddReg failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
                }
                $actual = $case.GetValue('FriendlyName')
                if($null -ne $name -and $actual -cne $name){throw "$section overwrote '$name' with '$actual'"}
                if($null -eq $name -and $null -ne $actual){throw "$section injected a placeholder before the driver supplied a name: '$actual'"}
                if(-not $case.GetValue('CLSID')){throw "$section lost KS proxy registration"}
                if($section -eq 'Oa2dpAudio.I.Topology') {
                    $ep = $case.OpenSubKey('EP\0')
                    try {
                        if(-not $ep -or $ep.GetValue('{1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E},7') -ne 1){throw 'Missing event-driven endpoint setting'}
                    } finally {if($ep){$ep.Dispose()}}
                }
            } finally {$case.Dispose()}
        }
    }
    'Production INF: fresh interfaces remain unnamed until driver publication; existing names and endpoint settings preserved (six isolated HKCU cases).'
} finally {
    $root.Dispose()
    [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($relative,$false)
    [EndpointInfTest]::SetupCloseInfFile($handle)
}
