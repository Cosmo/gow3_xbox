#requires -Version 7.2
[CmdletBinding()]
param([ValidateSet('ready','imports','boot','storage','memory')][string]$Mode='ready',[ValidateRange(1,40)][int]$WaitSeconds=5,[switch]$Register)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$layout=Join-Path $root '.local/build/layout'
foreach($process in @(Get-Process -Name gow3_uwp -ErrorAction SilentlyContinue)) {
    if($process.Path -eq (Join-Path $layout 'gow3_uwp.exe')) { Stop-Process -Id $process.Id; $process.WaitForExit(5000) | Out-Null }
}
if($Register) { Add-AppxPackage -Register (Join-Path $layout 'AppxManifest.xml') }
$package=Get-AppxPackage -Name Gow3.Uwp.Dev
if(!$package) { throw 'Register the development layout first using -Register' }
$state=Join-Path $env:LOCALAPPDATA "Packages/$($package.PackageFamilyName)/LocalState"
New-Item -ItemType Directory -Force $state | Out-Null
$settings=Get-Content -LiteralPath (Join-Path $root '.local/settings.json') -Raw | ConvertFrom-Json
[IO.File]::WriteAllText((Join-Path $state 'run.txt'),"$Mode`n$($settings.pc_dump)`n",[Text.UTF8Encoding]::new($false))
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
'@
}
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
