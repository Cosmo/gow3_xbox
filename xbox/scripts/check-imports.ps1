#requires -Version 7.2
param([string]$Folder='')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(!$Folder) { $Folder=Join-Path $root '.local/build/layout' }
$vs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -property installationPath
& "$vs/Common7/Tools/Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$llvm=Join-Path $vs 'VC/Tools/Llvm/x64/bin'
$library=Join-Path $env:WindowsSdkDir "Lib/$($env:WindowsSDKVersion.TrimEnd('\'))/um/x64/WindowsApp.lib"
$available=[Collections.Generic.HashSet[string]]::new()
foreach($line in (& "$llvm/llvm-nm.exe" $library 2>$null)) { if($line -match '__imp_(\S+)$') { [void]$available.Add($Matches[1]) } }
if(!$available.Count) { throw 'Could not read WindowsApp.lib; import verification cannot be skipped' }
$packaged=@((Get-ChildItem -LiteralPath $Folder -Filter '*.dll').Name)
$missing=@(foreach($binary in (Get-ChildItem -LiteralPath $Folder -File | Where-Object Extension -in '.exe','.dll')) {
    $dll=''
    $delayed=$false
    foreach($line in (& "$llvm/llvm-readobj.exe" --coff-imports $binary.FullName)) {
        if($line -match '^DelayImport \{') { $delayed=$true }
        elseif($line -match '^Import \{') { $delayed=$false }
        if($line -match '^\s*Name: (\S+)') { $dll=$Matches[1]; continue }
        if($line -notmatch '^\s*Symbol: ([^(\s]\S*)') { continue }
        $symbol=$Matches[1]
        if($delayed -and $binary.Name -eq 'vulkan_dzn.dll' -and $dll -match '^(USER32|GDI32|SHELL32|VERSION)\.dll$') { continue }
        if($dll -match '^(api-ms-win-crt-|vcruntime140(_1)?_app|msvcp140.*_app)' -or $packaged -contains $dll -or $available.Contains($symbol)) { continue }
        "$($binary.Name): $dll $symbol"
    }
})
if($missing.Count) { $missing; throw 'Package imports unavailable UWP APIs' }
Write-Output 'PASS: active package imports are UWP APIs, Store CRT, or packaged dependencies. Dozen desktop WSI DLLs remain delay-loaded.'
