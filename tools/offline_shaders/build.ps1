#requires -Version 7.2
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$repo=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$build=Join-Path $repo 'xbox/.local/build'
if(!(Test-Path -LiteralPath (Join-Path $build 'CMakeCache.txt'))) {
    throw 'Configure the UWP graphics build first with xbox/scripts/build.ps1 -Graphics.'
}
$vs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -property installationPath
& "$vs/Common7/Tools/Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$env:PATH="$vs/VC/Tools/Llvm/x64/bin;$vs/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin;C:/Program Files/CMake/bin;C:/Program Files/Ninja;$env:PATH"
& cmake --build $build --target gow3-offline-shader --parallel 3
if($LASTEXITCODE) { throw 'Headless shader compiler build failed.' }
