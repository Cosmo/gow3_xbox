#requires -Version 7.2
[CmdletBinding()]
param([switch]$Graphics)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$repo=Split-Path $root -Parent
$local=Join-Path $root '.local'
. (Join-Path $PSScriptRoot 'Gow3.Desktop.ps1')
$devPackage=Get-AppxPackage -Name 'GodOfWar3Launcher'
if($devPackage -and $devPackage.InstallLocation -and (Test-Path -LiteralPath $devPackage.InstallLocation) -and
   [IO.Path]::GetFullPath($devPackage.InstallLocation) -eq [IO.Path]::GetFullPath((Join-Path $local 'build/layout'))) {
    $how=Stop-Gow3DesktopApp $devPackage
    if($how -ne 'not running') { Write-Output "Running test app stop: $how." }
}
$config=Get-Content -LiteralPath (Join-Path $local 'settings.json') -Raw | ConvertFrom-Json
if($Graphics -and !(Test-Path -LiteralPath (Join-Path $local 'deps/ffmpeg-install/lib/libavcodec.a'))) {
    & (Join-Path $PSScriptRoot 'build-deps.ps1') -Python $config.python
    if($LASTEXITCODE) { throw 'Building the dependencies failed' }
}
$vs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -property installationPath
& "$vs/Common7/Tools/Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$env:PATH="$vs/VC/Tools/Llvm/x64/bin;$vs/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin;C:/Program Files/CMake/bin;C:/Program Files/Ninja;$env:PATH"
$build=Join-Path $local 'build'
# Dozen and DXC come from mesa_root (a local Mesa checkout), or from dozen_dll, mesa_license and dxc_root (the builds on GitHub).
$options=@("-DGOW3_BUILD_GRAPHICS=$($Graphics.IsPresent)","-DGOW3_MESA_ROOT=$($config.mesa_root)")
foreach($key in 'dozen_dll','mesa_license','dxc_root') {
    if($config.PSObject.Properties[$key] -and $config.$key) { $options+="-DGOW3_$($key.ToUpper())=$($config.$key)" }
}
& cmake -S $root -B $build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo @options
if($LASTEXITCODE) { throw 'UWP configure failed' }
& cmake --build $build --parallel 4 *> (Join-Path $local 'build.log')
if($LASTEXITCODE) { Get-Content (Join-Path $local 'build.log') -Tail 35; throw 'UWP build failed; details in xbox/.local/build.log' }
Write-Output 'UWP loader built; logs and layout are in xbox/.local.'
