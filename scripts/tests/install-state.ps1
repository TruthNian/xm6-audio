# SPDX-License-Identifier: MIT
# Exercise the installer's real state classifier without changing a device.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$path = Join-Path (Split-Path $PSScriptRoot -Parent) 'install.ps1'
$tokens=$null; $parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$parseErrors)
if ($parseErrors.Count) { throw 'Installer parse errors.' }
$definition=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Test-NativeDeviceState'},$true)
if (-not $definition) { throw 'Installer state classifier is missing.' }
. ([scriptblock]::Create($definition.Extent.Text))
$script:Selected='TEST\SELECTED'
function Get-PnpDeviceProperty {
    [CmdletBinding()]
    param([string]$InstanceId,[string]$KeyName)
    if ($InstanceId -ne $script:Selected) { throw 'Unexpected target.' }
    [pscustomobject]@{Data=$script:Properties[$KeyName]}
}
$cases=@(
    @{Name='started';Problem=0;Status=8;Restart=$false;Fails=$false},
    @{Name='earlier-rebind-pending';Problem=0;Status=0x108;Restart=$true;Fails=$false},
    @{Name='problem14-is-pending';Problem=14;Status=0;Restart=$true;Fails=$false},
    @{Name='failed-start';Problem=10;Status=0;Restart=$false;Fails=$true},
    @{Name='not-started';Problem=0;Status=0;Restart=$false;Fails=$true}
)
foreach($case in $cases) {
    $script:RestartRequired=$false
    $script:Properties=@{DEVPKEY_Device_ProblemCode=$case.Problem;DEVPKEY_Device_DevNodeStatus=$case.Status;DEVPKEY_Device_ProblemStatus=0}
    $failed=$false
    try { Test-NativeDeviceState 'TEST\SELECTED' } catch { $failed=$true }
    if ($failed -ne $case.Fails -or $script:RestartRequired -ne $case.Restart) {
        throw ('Incorrect install outcome for '+$case.Name)
    }
}
$updateDefinition=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Update-NativeRenderDriver'},$true)
if (-not $updateDefinition) { throw 'Render update procedure is missing.' }
. ([scriptblock]::Create($updateDefinition.Extent.Text))
# Compile the real native boundary, without invoking it. The scenario fixture
# uses a distinct fake type; these tests never bind a device.
$native=[regex]::Match($updateDefinition.Extent.Text,"(?s)Add-Type -TypeDefinition @'\r?\n(.*?)\r?\n'@").Groups[1].Value
if (-not $native) { throw 'Missing render-install native boundary.' }
Add-Type -TypeDefinition $native.Replace('namespace Xm6Audio.RenderBinding {','namespace Xm6Audio.RenderBindingCompileOnly {')
Add-Type @'
namespace Xm6Audio.RenderBinding {
 public static class Installer {
  public static int Calls;
  public static bool Reboot, Fail;
  public static string ExpectedInf, ExpectedInstance;
  public static bool Run(string instance,string inf,bool install) {
   if(instance!=ExpectedInstance || inf!=ExpectedInf || !install)
    throw new System.InvalidOperationException("Wrong render installation target");
   Calls++;
   if(Fail) throw new System.InvalidOperationException("Simulated native install failure");
   return Reboot;
  }
 }
}
'@
function Get-PnpDevice {
    [CmdletBinding()] param([string]$InstanceId,[switch]$PresentOnly)
    if ($InstanceId -ne $script:Selected -or -not $PresentOnly) { throw 'Unexpected render lookup.' }
    if ($script:MissingChild) { throw 'No present render device.' }
    [pscustomobject]@{InstanceId=$InstanceId}
}
function Resolve-Path {
    [CmdletBinding()] param([string]$LiteralPath)
    if ($LiteralPath -ne $script:ExpectedInf) { throw 'Unexpected INF resolution.' }
    [pscustomobject]@{Path=$LiteralPath}
}
function Get-FileHash {
    [CmdletBinding()] param([string]$LiteralPath,[string]$Algorithm)
    if ($Algorithm -ne 'SHA256') { throw 'Unexpected hash algorithm.' }
    $script:HashReads++
    if ($LiteralPath -eq $script:ExpectedInf) { return [pscustomobject]@{Hash='EXPECTED'} }
    if ($LiteralPath -ne (Join-Path $env:windir 'INF\oem123.inf')) { throw 'Unexpected published INF path.' }
    [pscustomobject]@{Hash=$(if($script:WrongPackage){'OTHER'}else{'EXPECTED'})}
}
$script:ExpectedInf='C:\SignedPackage\oa2dpaudio.inf'
$script:Selected='OA2DP\Render\001122334455'
[Xm6Audio.RenderBinding.Installer]::ExpectedInf=$script:ExpectedInf
[Xm6Audio.RenderBinding.Installer]::ExpectedInstance=$script:Selected
$updates=@(
    @{Name='same-package-still-reinstalls';Problem=0;Status=8;Restart=$false;Fails=$false;Calls=1},
    @{Name='installer-restart';Problem=0;Status=8;NativeRestart=$true;Restart=$true;Fails=$false;Calls=1},
    @{Name='earlier-restart-pending';Problem=14;Status=0;Restart=$true;Fails=$false;Calls=1},
    @{Name='missing-child';Problem=0;Status=8;Missing=$true;Restart=$false;Fails=$true;Calls=0},
    @{Name='wrong-hardware';Problem=0;Status=8;WrongHardware=$true;Restart=$false;Fails=$true;Calls=0},
    @{Name='wrong-instance';Problem=0;Status=8;WrongInstance=$true;Restart=$false;Fails=$true;Calls=0},
    @{Name='wrong-package';Problem=0;Status=8;Wrong=$true;Restart=$false;Fails=$true;Calls=1},
    @{Name='unsafe-inf';Problem=0;Status=8;Published='..\oem123.inf';Restart=$false;Fails=$true;Calls=1},
    @{Name='failed-start';Problem=10;Status=0;Restart=$false;Fails=$true;Calls=1},
    @{Name='native-install-error';Problem=0;Status=8;NativeFail=$true;Restart=$false;Fails=$true;Calls=1}
)
foreach($case in $updates) {
    $script:RestartRequired=$false
    $script:MissingChild=$case.ContainsKey('Missing') -and $case.Missing
    $script:WrongPackage=$case.ContainsKey('Wrong') -and $case.Wrong
    $script:HashReads=0
    [Xm6Audio.RenderBinding.Installer]::Calls=0
    [Xm6Audio.RenderBinding.Installer]::Reboot=$case.ContainsKey('NativeRestart') -and $case.NativeRestart
    [Xm6Audio.RenderBinding.Installer]::Fail=$case.ContainsKey('NativeFail') -and $case.NativeFail
    $published=if($case.ContainsKey('Published')){$case.Published}else{'oem123.inf'}
    $hardware=if($case.ContainsKey('WrongHardware')){'UNRELATED\Device'}else{'OA2DP\Render'}
    $target=if($case.ContainsKey('WrongInstance')){'USB\RADIO'}else{$script:Selected}
    $script:Properties=@{DEVPKEY_Device_ProblemCode=$case.Problem;DEVPKEY_Device_DevNodeStatus=$case.Status;DEVPKEY_Device_ProblemStatus=0;DEVPKEY_Device_DriverInfPath=$published;DEVPKEY_Device_HardwareIds=@($hardware)}
    $failed=$false
    try { Update-NativeRenderDriver $script:ExpectedInf $target } catch { $failed=$true }
    if ($failed -ne $case.Fails -or $script:RestartRequired -ne $case.Restart -or [Xm6Audio.RenderBinding.Installer]::Calls -ne $case.Calls) {
        throw ('Incorrect render installation outcome for '+$case.Name)
    }
    if ($case.Name -eq 'unsafe-inf' -and $script:HashReads -ne 0) { throw 'Unsafe INF reached file access.' }
}
Write-Output 'Installer state and selected-device reinstall checks passed without device changes.'
