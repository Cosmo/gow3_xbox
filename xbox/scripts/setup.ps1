#requires -Version 7.2
[CmdletBinding()]
param([Parameter(Mandatory)][string]$ResearchRoot,[Parameter(Mandatory)][string]$PcDump,
      [Parameter(Mandatory)][string]$XboxDump,[Parameter(Mandatory)][string]$Python,
      [string]$Console='series-x')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$local=Join-Path $root '.local'
if(Test-Path -LiteralPath (Join-Path $local 'settings.json')) { throw 'Local setup already exists; edit ignored settings.json deliberately instead of overwriting it' }
$source=Join-Path $ResearchRoot '.local/kyty-on-jetsku/3rdparty/winpthread'
$dependency=Join-Path $local 'deps/winpthread'
New-Item -ItemType Directory -Force (Join-Path $dependency 'include'),(Join-Path $dependency 'lib') | Out-Null
foreach($file in @('pthread.h','pthread_compat.h','pthread_time.h','pthread_signal.h','pthread_unistd.h','sched.h')) {
    Copy-Item -LiteralPath (Join-Path $source "include/$file") -Destination (Join-Path $dependency 'include')
}
Copy-Item -LiteralPath (Join-Path $source 'lib/libwinpthread.dll.a') -Destination (Join-Path $dependency 'lib')
Copy-Item -LiteralPath (Join-Path $source 'COPYING') -Destination $dependency
Copy-Item -LiteralPath (Join-Path $ResearchRoot '.local/deps/winpthread-ucrt/libwinpthread-1.dll') -Destination $dependency
$devices=Get-Content -LiteralPath (Join-Path $ResearchRoot 'devices.json') -Raw | ConvertFrom-Json -AsHashtable
if(!$devices.ContainsKey($Console)) { throw 'The requested console configuration does not exist in the reference project' }
$device=$devices[$Console]
if($device.auth -eq 'credential') {
    Copy-Item -LiteralPath (Join-Path $ResearchRoot ".local/$($device.credential_file)") -Destination $local
}
@{$Console=$device} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $local 'devices.json')
@{pc_dump=$PcDump; xbox_dump=$XboxDump; python=$Python} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $local 'settings.json')
Write-Output 'Private local setup created. The reference project was only read.'
