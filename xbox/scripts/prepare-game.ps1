#requires -Version 7.2
# Prepares the launcher's game files from your own God of War III Remastered dump (CUSA01715 and the
# other supported IDs). The launcher package contains no game code; it loads these files at run time,
# from LocalState\Prepared or from the gow3-prepared folder next to the dump. The default output is
# that folder, so preparing the dump on its USB drive once is enough for PC and Xbox.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Dump,[string]$Out='',[string]$Python='python')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$repo=Split-Path $root -Parent
if(!(Test-Path -LiteralPath (Join-Path $Dump 'eboot.bin'))) { throw "$Dump does not contain eboot.bin; pass the game's app0 folder" }
if(!$Out) { $Out=Join-Path $Dump 'gow3-prepared' }
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
