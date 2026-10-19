#requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateRange(1,600)][int]$Seconds=240,
    [ValidateSet('Gow3.Uwp.Dev','Gow3.Uwp.Smoke')][string]$PackageName='Gow3.Uwp.Smoke',
    [switch]$Capture,
    [switch]$StopAfter
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Gow3.Desktop.ps1')
$package=Get-AppxPackage -Name $PackageName
if(!$package) { throw "Package $PackageName is not registered" }
$controls=Join-Path $env:LOCALAPPDATA "Packages/$($package.PackageFamilyName)/LocalState/controls.txt"
$exe=Join-Path $package.InstallLocation 'gow3_uwp.exe'
$game=Get-Process -Name gow3_uwp -ErrorAction SilentlyContinue | Where-Object Path -EQ $exe | Select-Object -First 1
if(!$game) { throw "No running test process for $PackageName" }
$encoding=[Text.UTF8Encoding]::new($false)
$deadline=[DateTime]::UtcNow.AddSeconds($Seconds)
if($Capture) {
    Add-Type -AssemblyName System.Drawing
    $captures=Join-Path (Split-Path $PSScriptRoot -Parent) '.local/pc/auto-captures'
    New-Item -ItemType Directory -Force $captures | Out-Null
    $frame=Join-Path (Split-Path $controls -Parent) 'user/uwp-frame.bmp'
    $nextCapture=[DateTime]::UtcNow
    $captureIndex=0
    Write-Output "Keeping the latest 12 five-second captures in $captures"
}
$buttons=@('cross','square')
$buttonIndex=0
Write-Output "Alternating A and X every 0.5 seconds for $Seconds seconds ($PackageName)."
try {
    while([DateTime]::UtcNow -lt $deadline) {
        if($game.HasExited) { break }
        $step=[Diagnostics.Stopwatch]::StartNew()
        [IO.File]::WriteAllText($controls,$buttons[$buttonIndex],$encoding)
        $buttonIndex=($buttonIndex+1)%$buttons.Count
        Start-Sleep -Milliseconds 200
        [IO.File]::WriteAllText($controls,'',$encoding)
        if($Capture -and [DateTime]::UtcNow -ge $nextCapture -and (Test-Path -LiteralPath $frame)) {
            try {
                $bitmap=[Drawing.Image]::FromFile($frame)
                try { $bitmap.Save((Join-Path $captures ('frame-{0:D2}.png' -f ($captureIndex%12))),[Drawing.Imaging.ImageFormat]::Png) }
                finally { $bitmap.Dispose() }
                ++$captureIndex
            } catch { Write-Warning "Capture skipped: $($_.Exception.Message)" }
            $nextCapture=[DateTime]::UtcNow.AddSeconds(5)
        }
        $remaining=500-$step.ElapsedMilliseconds
        if($remaining -gt 0) { Start-Sleep -Milliseconds $remaining }
    }
} finally {
    [IO.File]::WriteAllText($controls,'',$encoding)
    if($StopAfter) { Write-Output "Test app stop: $(Stop-Gow3DesktopApp $package)." }
}
Write-Output 'Automatic input finished; all scripted buttons released.'
