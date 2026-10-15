#requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateRange(1,600)][int]$Seconds=240,
    [ValidateSet('Gow3.Uwp.Dev','Gow3.Uwp.Smoke')][string]$PackageName='Gow3.Uwp.Smoke'
)
$ErrorActionPreference='Stop'
$package=Get-AppxPackage -Name $PackageName
if(!$package) { throw "Package $PackageName is not registered" }
$controls=Join-Path $env:LOCALAPPDATA "Packages/$($package.PackageFamilyName)/LocalState/controls.txt"
$encoding=[Text.UTF8Encoding]::new($false)
$deadline=[DateTime]::UtcNow.AddSeconds($Seconds)
Write-Output "Pulsing A once per second for $Seconds seconds ($PackageName)."
try {
    while([DateTime]::UtcNow -lt $deadline) {
        [IO.File]::WriteAllText($controls,'cross',$encoding)
        Start-Sleep -Milliseconds 200
        [IO.File]::WriteAllText($controls,'',$encoding)
        Start-Sleep -Milliseconds 800
    }
} finally {
    [IO.File]::WriteAllText($controls,'',$encoding)
}
Write-Output 'Automatic input finished; all scripted buttons released.'
