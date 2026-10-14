#requires -Version 7.2
[CmdletBinding()]
param([switch]$Prepare)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$repo=Split-Path $root -Parent
$local=Join-Path $root '.local'
$config=Get-Content -LiteralPath (Join-Path $local 'settings.json') -Raw | ConvertFrom-Json
if($Prepare) {
    $prepared=Join-Path $local 'prepared'
    foreach($script in @('prepare.py','link_modules.py','content_profile.py')) {
        & $config.python (Join-Path $repo "scripts/$script") $config.pc_dump --out $prepared
        if($LASTEXITCODE) { throw "Guest preparation failed: $script" }
    }
}
$vs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -property installationPath
& "$vs/Common7/Tools/Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$env:PATH="$vs/VC/Tools/Llvm/x64/bin;C:/Program Files/CMake/bin;C:/Program Files/Ninja;$env:PATH"
$build=Join-Path $local 'build'
& cmake -S $root -B $build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo
if($LASTEXITCODE) { throw 'UWP configure failed' }
& cmake --build $build --parallel 4 *> (Join-Path $local 'build.log')
if($LASTEXITCODE) { Select-String -LiteralPath (Join-Path $local 'build.log') -Pattern 'error:|error LNK|undefined symbol|fatal error' | Select-Object -First 25 | ForEach-Object { $_.Line }; throw 'UWP build failed; details in xbox/.local/build.log' }
Write-Output 'UWP loader built; logs and layout are in xbox/.local.'
