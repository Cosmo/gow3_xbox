#requires -Version 7.2
# Closes the desktop UWP test app the way a user does. The app watches LocalState\quit.txt and closes its
# window itself; killing the process instead leaves its frame window open until Windows finishes tearing it down.
if(!('Gow3Windows' -as [type])) {
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class Gow3Windows {
    delegate bool EnumProc(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc proc, IntPtr data);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr window, StringBuilder text, int size);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    public static List<IntPtr> Find(string title) {
        var found = new List<IntPtr>();
        EnumWindows((window, data) => {
            var text = new StringBuilder(256);
            GetWindowText(window, text, text.Capacity);
            if (IsWindowVisible(window) && text.ToString() == title) found.Add(window);
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static void Close(IntPtr window) { PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero); }
}
'@
}

function Get-Gow3DesktopProcess([string]$Executable) {
    $target=[IO.Path]::GetFullPath($Executable)
    @(Get-Process -Name gow3_uwp -ErrorAction SilentlyContinue | Where-Object {
        !$_.HasExited -and $_.Path -and [IO.Path]::GetFullPath($_.Path) -eq $target })
}

function Get-Gow3WindowTitle($Package) {
    $manifest=[xml](Get-Content -LiteralPath (Join-Path $Package.InstallLocation 'AppxManifest.xml') -Raw)
    $manifest.Package.Applications.Application.VisualElements.DisplayName
}

function Wait-Gow3DesktopExit([string]$Executable,[int]$Seconds) {
    $deadline=[DateTime]::UtcNow.AddSeconds($Seconds)
    while([DateTime]::UtcNow -lt $deadline) {
        if(!(Get-Gow3DesktopProcess $Executable).Count) { return $true }
        Start-Sleep -Milliseconds 250
    }
    !(Get-Gow3DesktopProcess $Executable).Count
}

# Returns how the app closed: 'not running', 'closed itself', 'window closed' or 'killed'.
function Stop-Gow3DesktopApp($Package,[int]$QuitSeconds=30,[int]$WindowSeconds=15) {
    $exe=Join-Path $Package.InstallLocation 'gow3_uwp.exe'
    $title=Get-Gow3WindowTitle $Package
    $how='not running'
    if((Get-Gow3DesktopProcess $exe).Count) {
        $state=Join-Path $env:LOCALAPPDATA "Packages/$($Package.PackageFamilyName)/LocalState"
        [IO.File]::WriteAllText((Join-Path $state 'quit.txt'),'',[Text.UTF8Encoding]::new($false))
        $how='closed itself'
        if(!(Wait-Gow3DesktopExit $exe $QuitSeconds)) {
            $how='window closed'
            foreach($window in [Gow3Windows]::Find($title)) { [Gow3Windows]::Close($window) }
            if(!(Wait-Gow3DesktopExit $exe $WindowSeconds)) {
                $how='killed'
                Get-Gow3DesktopProcess $exe | Stop-Process -Force -ErrorAction SilentlyContinue
                $null=Wait-Gow3DesktopExit $exe 10
            }
        }
    }
    # A process that is gone can still leave its frame window behind; close it like a user would.
    $deadline=[DateTime]::UtcNow.AddSeconds(5)
    while(([Gow3Windows]::Find($title)).Count -and [DateTime]::UtcNow -lt $deadline) {
        foreach($window in [Gow3Windows]::Find($title)) { [Gow3Windows]::Close($window) }
        Start-Sleep -Milliseconds 250
    }
    if((Get-Gow3DesktopProcess $exe).Count) { throw "The test app is still running after: $how" }
    if(([Gow3Windows]::Find($title)).Count) { throw "The test app's window is still open after: $how" }
    $how
}
