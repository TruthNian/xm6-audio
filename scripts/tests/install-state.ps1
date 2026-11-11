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
function Get-PnpDeviceProperty {
    [CmdletBinding()]
    param([string]$InstanceId,[string]$KeyName)
    if ($InstanceId -ne 'TEST\SELECTED') { throw 'Unexpected target.' }
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
function pnputil.exe {
    param([string]$AddDriver,[string]$InfPath,[string]$Install)
    if ($AddDriver -ne '/add-driver' -or $InfPath -ne $script:ExpectedInf -or $Install -ne '/install') { throw 'Unexpected PnP command.' }
    # Keep fake native exits inside this test script. CI's calling PowerShell
    # checks its own LASTEXITCODE after all scripts have returned.
    $script:LASTEXITCODE=$script:PnpResult
}
function Get-PnpDevice {
    [CmdletBinding()] param([string]$InstanceId,[switch]$PresentOnly)
    if ($InstanceId -ne 'TEST\SELECTED' -or -not $PresentOnly) { throw 'Unexpected render device lookup.' }
    $script:DeviceReads++
    if ($script:MissingChild) { throw 'No present render device.' }
    [pscustomobject]@{InstanceId=$InstanceId}
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
$updates=@(
    @{Name='updated';Exit=0;Problem=0;Status=8;Restart=$false;Fails=$false;Reads=0},
    @{Name='installer-restart';Exit=3010;Problem=0;Status=8;Restart=$true;Fails=$false;Reads=0},
    @{Name='unchanged-verified';Exit=259;Problem=0;Status=8;Restart=$false;Fails=$false;Reads=1},
    @{Name='unchanged-restart-pending';Exit=259;Problem=14;Status=0;Restart=$true;Fails=$false;Reads=1},
    @{Name='unchanged-missing-child';Exit=259;Problem=0;Status=8;Missing=$true;Restart=$false;Fails=$true;Reads=1},
    @{Name='unchanged-wrong-package';Exit=259;Problem=0;Status=8;Wrong=$true;Restart=$false;Fails=$true;Reads=1},
    @{Name='unchanged-unsafe-inf';Exit=259;Problem=0;Status=8;Published='..\oem123.inf';Restart=$false;Fails=$true;Reads=1},
    @{Name='unchanged-failed-start';Exit=259;Problem=10;Status=0;Restart=$false;Fails=$true;Reads=1},
    @{Name='real-install-error';Exit=5;Problem=0;Status=8;Restart=$false;Fails=$true;Reads=0}
)
foreach($case in $updates) {
    $script:RestartRequired=$false
    $script:PnpResult=$case.Exit
    $script:MissingChild=$case.ContainsKey('Missing') -and $case.Missing
    $script:WrongPackage=$case.ContainsKey('Wrong') -and $case.Wrong
    $script:DeviceReads=0; $script:HashReads=0
    $published=if($case.ContainsKey('Published')){$case.Published}else{'oem123.inf'}
    $script:Properties=@{DEVPKEY_Device_ProblemCode=$case.Problem;DEVPKEY_Device_DevNodeStatus=$case.Status;DEVPKEY_Device_ProblemStatus=0;DEVPKEY_Device_DriverInfPath=$published}
    $failed=$false
    try { Update-NativeRenderDriver $script:ExpectedInf 'TEST\SELECTED' } catch { $failed=$true }
    if ($failed -ne $case.Fails -or $script:RestartRequired -ne $case.Restart -or $script:DeviceReads -ne $case.Reads) {
        throw ('Incorrect render update outcome for '+$case.Name)
    }
    if ($case.Name -eq 'unchanged-unsafe-inf' -and $script:HashReads -ne 0) { throw 'Unsafe INF reached file access.' }
}
Write-Output 'Installer state and unchanged-package checks passed without device changes.'
