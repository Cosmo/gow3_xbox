#requires -Version 7.2
# Creates the ignored local settings (.local/settings.json) and, if missing, a console list from
# devices.example.json, then fetches and builds the pinned third-party dependencies.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$PcDump,[string]$XboxDump='',[string]$Python='python',[string]$MesaRoot='')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$local=Join-Path $root '.local'
New-Item -ItemType Directory -Force $local | Out-Null
$settings=Join-Path $local 'settings.json'
if(Test-Path -LiteralPath $settings) { throw 'Local setup already exists; edit .local/settings.json deliberately instead of overwriting it' }
@{pc_dump=$PcDump; xbox_dump=$XboxDump; python=$Python; mesa_root=$MesaRoot} | ConvertTo-Json | Set-Content -LiteralPath $settings
$devices=Join-Path $local 'devices.json'
if(!(Test-Path -LiteralPath $devices)) {
    Copy-Item -LiteralPath (Join-Path $root 'devices.example.json') -Destination $devices
    Write-Output 'Console list created from devices.example.json; set your console address in .local/devices.json.'
}
& (Join-Path $PSScriptRoot 'build-deps.ps1') -Python $Python
if($LASTEXITCODE) { throw 'Building the dependencies failed' }
Write-Output 'Private local setup created.'
