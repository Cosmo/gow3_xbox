#requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateRange(1,600)][int]$Seconds=240,
    [ValidateSet('Gow3.Uwp.Dev','Gow3.Uwp.Smoke')][string]$PackageName='Gow3.Uwp.Smoke',
    [switch]$Capture,
    [switch]$StopAfter
)
$ErrorActionPreference='Stop'
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
Write-Output "Pulsing A once per second for $Seconds seconds ($PackageName)."
try {
    while([DateTime]::UtcNow -lt $deadline) {
        if($game.HasExited) { break }
        [IO.File]::WriteAllText($controls,'cross',$encoding)
        Start-Sleep -Milliseconds 200
        [IO.File]::WriteAllText($controls,'',$encoding)
        Start-Sleep -Milliseconds 800
        if($Capture -and [DateTime]::UtcNow -ge $nextCapture -and (Test-Path -LiteralPath $frame)) {
            try {
                $bitmap=[Drawing.Image]::FromFile($frame)
                try { $bitmap.Save((Join-Path $captures ('frame-{0:D2}.png' -f ($captureIndex%12))),[Drawing.Imaging.ImageFormat]::Png) }
                finally { $bitmap.Dispose() }
                ++$captureIndex
            } catch { Write-Warning "Capture skipped: $($_.Exception.Message)" }
            $nextCapture=[DateTime]::UtcNow.AddSeconds(5)
        }
    }
} finally {
    [IO.File]::WriteAllText($controls,'',$encoding)
    if($StopAfter -and !$game.HasExited) {
        Stop-Process -Id $game.Id
        $game.WaitForExit(5000) | Out-Null
    }
}
Write-Output 'Automatic input finished; all scripted buttons released.'
