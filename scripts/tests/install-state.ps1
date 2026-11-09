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
Write-Output 'Installer state checks passed without device changes.'
