# SPDX-License-Identifier: MIT
# Creates a development/submission package. Never signs or installs anything.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot -Parent
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Use a new, empty output directory.' }
New-Item -ItemType Directory -Path $output | Out-Null
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$inf2cat = Get-ChildItem "$kits\bin\*\x86\Inf2Cat.exe" | Sort-Object FullName | Select-Object -Last 1
$infverif = Get-ChildItem "$kits\Tools\*\x64\infverif.exe" -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1
if (-not $infverif) {
    $infverif = Get-ChildItem "$kits\bin\*\x86\infverif.exe" | Sort-Object FullName | Select-Object -Last 1
}
if (-not $inf2cat -or -not $infverif) { throw 'WDK Inf2Cat and InfVerif are required.' }
foreach ($driver in @('oa2dp','oa2dpaudio')) {
    $dst = Join-Path $output "drivers\$driver"
    New-Item -ItemType Directory -Path $dst -Force | Out-Null
    Copy-Item -LiteralPath "$repo\driver\$driver\$driver.inf" -Destination $dst
    foreach ($ext in @('sys','pdb')) {
        Copy-Item -LiteralPath "$build\Release\$driver.$ext" -Destination $dst
    }
    if ($driver -eq 'oa2dp') {
        foreach ($file in @('oa2dpsvc.exe','oa2dpctl.exe','ldac.dll','sbc.dll')) {
            Copy-Item -LiteralPath "$build\Release\$file" -Destination $dst
        }
    }
    & $infverif.FullName /w "$dst\$driver.inf"
    if ($LASTEXITCODE -ne 0) { throw "InfVerif failed for $driver ($LASTEXITCODE)." }
    & $inf2cat.FullName "/driver:$dst" /os:10_VB_X64,10_CO_X64,10_NI_X64,10_GE_X64 /uselocaltime
    if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed for $driver ($LASTEXITCODE)." }
}
$licenses = Join-Path $output 'licenses'
New-Item -ItemType Directory -Path $licenses | Out-Null
Copy-Item -LiteralPath "$repo\LICENSE" -Destination "$licenses\XM6Audio-MIT.txt"
Copy-Item -LiteralPath "$repo\driver\oa2dpaudio\LICENSE.MS-PL" -Destination "$licenses\SYSVAD-MS-PL.txt"
Copy-Item -LiteralPath "$repo\third_party\libldac\LICENSE" -Destination "$licenses\libldac-Apache-2.0.txt"
Copy-Item -LiteralPath "$repo\third_party\libldac\NOTICE" -Destination "$licenses\libldac-NOTICE.txt"
Copy-Item -LiteralPath "$repo\third_party\sbc\COPYING.LIB" -Destination "$licenses\libsbc-LGPL-2.1.txt"
Copy-Item -LiteralPath "$repo\THIRD_PARTY.md" -Destination $licenses
Copy-Item -LiteralPath "$PSScriptRoot\verify-package.ps1" -Destination $output
Copy-Item -LiteralPath "$PSScriptRoot\install.ps1" -Destination $output
Copy-Item -LiteralPath "$PSScriptRoot\restore-windows.ps1" -Destination $output
$files = @{}
Get-ChildItem -LiteralPath $output -Recurse -File | ForEach-Object {
    $relative = $_.FullName.Substring($output.Length + 1).Replace('\','/')
    $files[$relative] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
$manifest = [ordered]@{
    project = 'XM6 Audio'; version = '0.3.6'; architecture = 'x64'
    commit = (& git -C $repo rev-parse HEAD)
    upstream = 'RafiMota/open-a2dp@167ebc0e2c89fec570eec535fefdade72bd9e416'
    libldac = (& git -C "$repo\third_party\libldac" rev-parse HEAD)
    libsbc = (& git -C "$repo\third_party\sbc" rev-parse HEAD)
    findwdk = (& git -C "$repo\third_party\FindWDK" rev-parse HEAD)
    signing = 'UNSIGNED: development/submission candidate, not host-installable'
    files = $files
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$output\manifest.json" -Encoding UTF8
Write-Host "Created unsigned native candidate: $output"
