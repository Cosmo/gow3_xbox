#requires -Version 7.2
[CmdletBinding()]
param([ValidateSet('ready','imports','boot','storage','prepare','memory','graphics','texture','player')][string]$Mode='ready',[ValidateRange(1,40)][int]$WaitSeconds=5,[switch]$Register,[switch]$Isolated,[ValidateSet('Gow3.Uwp.Dev','Gow3.Uwp.Smoke')][string]$PackageName='Gow3.Uwp.Dev')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Gow3.Desktop.ps1')
$root=Split-Path $PSScriptRoot -Parent
$layout=Join-Path $root '.local/build/layout'
if($PackageName -eq 'Gow3.Uwp.Smoke') { $layout=Join-Path $root '.local/pc-layout' }
$testExes=@(
    [IO.Path]::GetFullPath((Join-Path $root '.local/build/layout/gow3_uwp.exe')),
    [IO.Path]::GetFullPath((Join-Path $root '.local/pc-layout/gow3_uwp.exe'))
)
foreach($testPackage in @(Get-AppxPackage -Name 'Gow3.Uwp.Dev'; Get-AppxPackage -Name 'Gow3.Uwp.Smoke')) {
    if([IO.Path]::GetFullPath((Join-Path $testPackage.InstallLocation 'gow3_uwp.exe')) -in $testExes) {
        $how=Stop-Gow3DesktopApp $testPackage
        if($how -ne 'not running') { Write-Output "Previous test app ($($testPackage.Name)) stop: $how." }
    }
}
if($Isolated) {
    $sourceLayout=$layout
    $layout=Join-Path $root '.local/pc-layout'
    New-Item -ItemType Directory -Force $layout | Out-Null
    foreach($name in @('AppxManifest.xml','gow3_uwp.exe','libwinpthread-1.dll','vulkan_dzn.dll','dxil.dll','Assets','Patches','ThirdParty')) {
        Copy-Item -LiteralPath (Join-Path $sourceLayout $name) -Destination $layout -Recurse -Force
    }
    $symbols=Join-Path $root '.local/build/gow3_uwp.pdb'
    if(Test-Path -LiteralPath $symbols) { Copy-Item -LiteralPath $symbols -Destination $layout -Force }
    $packageName='Gow3.Uwp.Smoke'
    $manifest=Join-Path $layout 'AppxManifest.xml'
    # A rising version (as package.ps1 uses) so registering never fails against a newer test package.
    $now=[DateTime]::UtcNow
    $version='1.0.{0}.{1}' -f ($now.Year%100*1000+$now.DayOfYear),([int]$now.ToString('HHmm'))
    $xml=[IO.File]::ReadAllText($manifest).Replace('Name="Gow3.Uwp.Dev"','Name="Gow3.Uwp.Smoke"').Replace('Version="1.0.0.0"',"Version=`"$version`"")
    [IO.File]::WriteAllText($manifest,$xml,[Text.UTF8Encoding]::new($false))
    $Register=$true
}
if($Register) { Add-AppxPackage -Register (Join-Path $layout 'AppxManifest.xml') -ForceApplicationShutdown }
$package=Get-AppxPackage -Name $packageName
if(!$package) { throw 'Register the development layout first using -Register' }
$state=Join-Path $env:LOCALAPPDATA "Packages/$($package.PackageFamilyName)/LocalState"
New-Item -ItemType Directory -Force $state | Out-Null
$settings=Get-Content -LiteralPath (Join-Path $root '.local/settings.json') -Raw | ConvertFrom-Json
# player: no run.txt, the app finds the dump on a USB drive and asks before preparing, as for players.
if($Mode -eq 'player') { Remove-Item -LiteralPath (Join-Path $state 'run.txt') -ErrorAction SilentlyContinue }
else { [IO.File]::WriteAllText((Join-Path $state 'run.txt'),"$Mode`n$($settings.pc_dump)`n",[Text.UTF8Encoding]::new($false)) }
if(!('Gow3Activation' -as [type])) {
Add-Type @'
using System;
using System.Runtime.InteropServices;
[ComImport, Guid("2e941141-7f97-4756-ba1d-9decde894a3d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IGow3Activation {
    [PreserveSig] int ActivateApplication([MarshalAs(UnmanagedType.LPWStr)] string id,[MarshalAs(UnmanagedType.LPWStr)] string args,uint options,out uint pid);
    [PreserveSig] int ActivateForFile(IntPtr a,IntPtr b,IntPtr c,out uint pid);
    [PreserveSig] int ActivateForProtocol(IntPtr a,IntPtr b,out uint pid);
}
public static class Gow3Activation {
    public static uint Run(string id) {
        var manager=(IGow3Activation)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("45ba127d-10a8-46ea-8ab7-56ea9078943c")));
        uint pid;
        int result=manager.ActivateApplication(id,"",0,out pid);
        Marshal.ReleaseComObject(manager);
        Marshal.ThrowExceptionForHR(result);
        return pid;
    }
}
[ComImport, Guid("F27C3930-8029-4AD1-94E3-3DBA417810C1"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IGow3PackageDebug {
    [PreserveSig] int EnableDebugging([MarshalAs(UnmanagedType.LPWStr)] string package,IntPtr debugger,IntPtr environment);
    [PreserveSig] int DisableDebugging([MarshalAs(UnmanagedType.LPWStr)] string package);
    [PreserveSig] int Suspend([MarshalAs(UnmanagedType.LPWStr)] string package);
    [PreserveSig] int Resume([MarshalAs(UnmanagedType.LPWStr)] string package);
}
public static class Gow3PackageDebug {
    public static void Enable(string package) {
        var manager=(IGow3PackageDebug)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("B1AEC16F-2383-4852-B0E9-8F0B1DC66B4D")));
        try {
            Marshal.ThrowExceptionForHR(manager.EnableDebugging(package,IntPtr.Zero,IntPtr.Zero));
        } finally { Marshal.ReleaseComObject(manager); }
    }
    public static void Disable(string package) {
        var manager=(IGow3PackageDebug)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("B1AEC16F-2383-4852-B0E9-8F0B1DC66B4D")));
        try { Marshal.ThrowExceptionForHR(manager.DisableDebugging(package)); }
        finally { Marshal.ReleaseComObject(manager); }
    }
}
'@
}
[Gow3PackageDebug]::Enable($package.PackageFullName)
$started=[DateTime]::UtcNow
$pidValue=[Gow3Activation]::Run("$($package.PackageFamilyName)!App")
Write-Output "PC UWP activated (process $pidValue), mode $Mode."
Start-Sleep -Seconds $WaitSeconds
$log=Join-Path $state 'loader.log'
if(!(Test-Path -LiteralPath $log)) { throw 'No loader log was created; check application activation events' }
if((Get-Item -LiteralPath $log).LastWriteTimeUtc -lt $started.AddSeconds(-1)) { throw 'Loader log is stale; this activation did not write a new log' }
$output=Join-Path $root '.local/pc'
New-Item -ItemType Directory -Force $output | Out-Null
$destination=Join-Path $output ("{0}-{1}.log" -f $Mode,[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'))
$reader=[IO.FileStream]::new($log,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
try {
    $writer=[IO.File]::Create($destination)
    try { $reader.CopyTo($writer) } finally { $writer.Dispose() }
} finally { $reader.Dispose() }
Get-Content -LiteralPath $destination -Tail 30
$errorLog=Join-Path $state 'errors.log'
if(Test-Path -LiteralPath $errorLog) {
    $reader=[IO.FileStream]::new($errorLog,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    try {
        $writer=[IO.File]::Create($destination.Replace('.log','-errors.log'))
        try { $reader.CopyTo($writer) } finally { $writer.Dispose() }
    } finally { $reader.Dispose() }
    Get-Content -LiteralPath $destination.Replace('.log','-errors.log') -Tail 15
}
if($Mode -notin 'boot','player') {
    Write-Output "Test app stop: $(Stop-Gow3DesktopApp $package)."
}
