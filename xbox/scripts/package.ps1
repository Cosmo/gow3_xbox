#requires -Version 7.2
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$local=Join-Path $root '.local'
$layout=Join-Path $local 'build/layout'
& (Join-Path $PSScriptRoot 'check-imports.ps1') -Folder $layout
& (Join-Path $PSScriptRoot 'assets.ps1')
$vs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -property installationPath
& "$vs/Common7/Tools/Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$sdk=Join-Path $env:WindowsSdkVerBinPath 'x64'
$out=Join-Path $local ('packages/'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
$stage=Join-Path $out 'layout'
New-Item -ItemType Directory -Force $stage | Out-Null
foreach($name in @('AppxManifest.xml','gow3_uwp.exe','libwinpthread-1.dll','Assets','Prepared','ThirdParty')) {
    Copy-Item -LiteralPath (Join-Path $layout $name) -Destination $stage -Recurse
}
foreach($name in @('vulkan_dzn.dll','dxil.dll')) {
    if(Test-Path -LiteralPath (Join-Path $layout $name)) {
        Copy-Item -LiteralPath (Join-Path $layout $name) -Destination $stage
    }
}
$certificate=Get-ChildItem Cert:/CurrentUser/My | Where-Object { $_.Subject -eq 'CN=Gow3UwpDev' -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddDays(7) } | Select-Object -First 1
if(!$certificate) {
    $certificate=New-SelfSignedCertificate -Subject 'CN=Gow3UwpDev' -Type Custom -KeyUsage DigitalSignature -CertStoreLocation Cert:/CurrentUser/My -FriendlyName 'God of War UWP Dev' -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3','2.5.29.19={text}CA=false')
}
$version='1.0.{0}.{1}' -f ([DateTime]::UtcNow.Year%100*1000+[DateTime]::UtcNow.DayOfYear),([int][DateTime]::UtcNow.ToString('HHmm'))
$manifest=Join-Path $stage 'AppxManifest.xml'
$content=(Get-Content -LiteralPath $manifest -Raw).Replace('Version="1.0.0.0"',"Version=`"$version`"")
[IO.File]::WriteAllText($manifest,$content,[Text.UTF8Encoding]::new($false))
$package=Join-Path $out "Gow3.Uwp.Dev_$version`_x64.appx"
& "$sdk/makeappx.exe" pack /d $stage /p $package /o *> (Join-Path $out 'pack.log')
if($LASTEXITCODE) { Get-Content (Join-Path $out 'pack.log') -Tail 12; throw 'Package creation failed' }
& "$sdk/signtool.exe" sign /q /fd SHA256 /sha1 $certificate.Thumbprint $package
if($LASTEXITCODE) { throw 'Development signing failed' }
[IO.File]::WriteAllText((Join-Path $local 'latest-package.txt'),$package,[Text.UTF8Encoding]::new($false))
Write-Output "Signed development package created: $version."
