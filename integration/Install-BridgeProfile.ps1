[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidatePattern('^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$')][string]$DeviceAddress,
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [string]$RuntimeDirectory=(Join-Path $env:LOCALAPPDATA 'Programs\XM6Audio')
)
$ErrorActionPreference='Stop'
$build=(Resolve-Path -LiteralPath $BuildDirectory).Path
foreach($file in @('bluecaster-core.exe','bluecaster-driver.exe','bluecaster.exe')) {
    if(-not(Test-Path -LiteralPath (Join-Path $build $file))){throw "Missing runtime file: $file"}
}
if(Get-Process bluecaster-core -ErrorAction SilentlyContinue){throw 'Close the existing BlueCaster core before installing. No process has been terminated.'}
$dir=Join-Path $env:APPDATA 'BlueCaster'
$backup=Join-Path $dir ('backup\xm6-native-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
$null=New-Item -ItemType Directory -Force -Path $backup,$RuntimeDirectory
foreach($file in @('settings.json','profiles.json')) {
    $path=Join-Path $dir $file
    if(Test-Path -LiteralPath $path){Copy-Item -LiteralPath $path -Destination (Join-Path $backup $file)}
}
$run='HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$previous=(Get-ItemProperty -LiteralPath $run -Name BlueCaster -ErrorAction SilentlyContinue).BlueCaster
@{PreviousRun=$previous;RuntimeDirectory=$RuntimeDirectory} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $backup 'install.json') -Encoding utf8
foreach($file in @('bluecaster-core.exe','bluecaster-driver.exe','bluecaster.exe')) {
    $source=Join-Path $build $file
    $dest=Join-Path $RuntimeDirectory $file
    if([IO.Path]::GetFullPath($source) -ne [IO.Path]::GetFullPath($dest)){Copy-Item -LiteralPath $source -Destination $dest -Force}
}
function Set-Field($obj,[string]$name,$value){$obj | Add-Member -MemberType NoteProperty -Name $name -Value $value -Force}
function Write-Json($obj,[string]$path){[IO.File]::WriteAllText($path,($obj | ConvertTo-Json -Depth 20),[Text.UTF8Encoding]::new($false))}
$settingsPath=Join-Path $dir 'settings.json'
$s=if(Test-Path -LiteralPath $settingsPath){Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json}else{[pscustomobject]@{schema_version=8}}
$fields=@{start_with_windows=$true;start_minimized=$true;auto_reconnect=$true;volume_sync=$true;output_mute=$true;auto_codec=$false;check_updates_on_startup=$false;last_profile='XM6 LDAC 990';le_audio_experimental=$false;awaiting_vb_cable=$false}
foreach($entry in $fields.GetEnumerator()){Set-Field $s $entry.Key $entry.Value}
Write-Json $s $settingsPath
$profilesPath=Join-Path $dir 'profiles.json'
$p=if(Test-Path -LiteralPath $profilesPath){Get-Content -LiteralPath $profilesPath -Raw | ConvertFrom-Json}else{[pscustomobject]@{profiles=@()}}
$keep=@($p.profiles | Where-Object {$_.name -ne 'XM6 LDAC 990'})
$profile=[ordered]@{name='XM6 LDAC 990';device_address=$DeviceAddress.ToUpperInvariant();device_name='WH-1000XM6';codec='ldac';quality='hq';sample_rate=48000;bit_depth=24;abr=$false;codec_forced=$true;codec_fallback=$false;capture_mode='loopback';auto_switch_device=$false;le_audio=$false;codec_order=@('ldac')}
Set-Field $p 'profiles' @($keep+[pscustomobject]$profile)
Write-Json $p $profilesPath
$null=New-Item -Path $run -Force
$native=Join-Path $RuntimeDirectory 'bluecaster-core.exe'
$null=New-ItemProperty -LiteralPath $run -Name BlueCaster -PropertyType String -Value ('"'+$native+'" --background') -Force
Start-Process -FilePath $native -ArgumentList '--background' -WindowStyle Hidden
Write-Output 'Installed the experimental WinUSB bridge profile and its startup entry. This is not a native Windows Bluetooth audio driver.'
Write-Output ('Configuration backup: '+$backup)
