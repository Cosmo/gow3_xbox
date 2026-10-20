#requires -Version 7.2
# Makes the launcher's game files from a God of War III Remastered dump with the Python reference
# scripts. The app makes the same files itself on its first start (platform/prepare_game.cpp); this
# script is for developers, to compare the two (pc-smoke.ps1 -Mode prepare writes the app's copy).
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Dump,[string]$Out='',[string]$Python='python')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$repo=Split-Path $root -Parent
if(!(Test-Path -LiteralPath (Join-Path $Dump 'eboot.bin'))) { throw "$Dump does not contain eboot.bin; pass the game's app0 folder" }
if(!$Out) { $Out=Join-Path $root '.local/prepared' }
New-Item -ItemType Directory -Force $Out | Out-Null
$marker=Join-Path $Out 'format.txt'
if(Test-Path -LiteralPath $marker) { Remove-Item -LiteralPath $marker }
foreach($script in @('prepare.py','link_modules.py','content_profile.py')) {
    & $Python (Join-Path $repo "scripts/$script") $Dump --out $Out
    if($LASTEXITCODE) { throw "Game preparation failed: $script" }
}
& $Python (Join-Path $PSScriptRoot 'prepare-graphics.py') --out $Out
if($LASTEXITCODE) { throw 'Graphics patch preparation failed' }
$format=(Get-Content -LiteralPath (Join-Path $root 'prepared-format.txt') -TotalCount 1).Trim()
[IO.File]::WriteAllText($marker,"gow3-uwp-prepared $format`n",[Text.Encoding]::ASCII)
Write-Output "Prepared game files (format $format) written to $Out"
