#requires -Version 7.2
# Uploads prepared game files (prepare-game.ps1 output) into the installed app's LocalState\Prepared on
# a console. Launch the app once after installing it: that creates LocalState and the Prepared folder.
# Players can skip this and keep the files in gow3-prepared next to the dump on the USB drive.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Console,[Parameter(Mandatory)][string]$App,[string]$Folder='')
$ErrorActionPreference='Stop'
if(!$Folder) { $Folder=Join-Path (Split-Path $PSScriptRoot -Parent) '.local/prepared' }
foreach($name in @('boot-linked.bin','content.bin','patches-native.bin','patches-720p.bin','format.txt')) {
    $file=Join-Path $Folder $name
    if(!(Test-Path -LiteralPath $file)) { throw "$file is missing; run prepare-game.ps1 first" }
    & (Join-Path $PSScriptRoot 'xbox-upload.ps1') -Console $Console -App $App -File $file `
        -RemotePath "LocalState/Prepared/$name" -Overwrite -TimeoutSeconds 900 -Json | Out-Null
    if($LASTEXITCODE) { throw "Uploading $name failed" }
    Write-Output "Uploaded $name"
}
