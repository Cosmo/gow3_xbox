#requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$App,
    [ValidateRange(1,600)][int]$Seconds=240,
    [ValidateRange(1,30)][int]$RequestTimeoutSeconds=5,
    [string]$Console='default',
    [switch]$NoCapture,
    [switch]$StopAfter
)
. (Join-Path $PSScriptRoot 'Xbox.Common.ps1')
$portal=Open-XboxPortal $Console '' $false 15
$package=Find-XboxApp $portal $App
$output=Assert-XboxPath (Join-Path $script:XboxOutputRoot ('auto-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfff')))
New-Item -ItemType Directory -Path $output | Out-Null
$buttons=@(Join-Path $output 'cross.txt'; Join-Path $output 'square.txt')
$neutral=Join-Path $output 'neutral.txt'
[IO.File]::WriteAllText($buttons[0],'cross')
[IO.File]::WriteAllText($buttons[1],'square')
[IO.File]::WriteAllText($neutral,'')
$query=Get-XboxFileQuery $package 'LocalState/controls.txt'
$query.extract='false'
function Set-RequestDeadline {
    $cap=[DateTime]::UtcNow.AddSeconds($RequestTimeoutSeconds)
    $portal.Deadline=if($cap -lt $deadline){$cap}else{$deadline}
}
function Send-Buttons([string]$File) {
    Set-RequestDeadline
    $form=New-XboxMultipart @($File) @('controls.txt')
    $null=Invoke-XboxRequest $portal POST '/api/filesystem/apps/file' $query $form @(200,201,204)
}
$deadline=[DateTime]::UtcNow.AddSeconds($Seconds)
$nextCapture=[DateTime]::UtcNow
$captureIndex=0
$buttonIndex=0
Write-Output "Alternating A and X every 0.5 seconds for $Seconds seconds; evidence: $output"
Write-Output "Input deadline: $($deadline.ToString('o')); each portal request limited to $RequestTimeoutSeconds seconds."
if($NoCapture) { Write-Output 'Screenshot capture disabled for performance measurement.' }
try {
    while([DateTime]::UtcNow -lt $deadline) {
        $step=[Diagnostics.Stopwatch]::StartNew()
        Send-Buttons $buttons[$buttonIndex]
        $buttonIndex=($buttonIndex+1)%$buttons.Count
        Start-Sleep -Milliseconds 200
        Send-Buttons $neutral
        if(!$NoCapture -and [DateTime]::UtcNow -ge $nextCapture) {
            $path=Join-Path $output ('frame-{0:D2}.png' -f ($captureIndex%12))
            if(Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
            Set-RequestDeadline
            $null=Invoke-XboxRequest $portal GET '/ext/screenshot' @{} $null @(200) $path
            ++$captureIndex
            $nextCapture=[DateTime]::UtcNow.AddSeconds(5)
        }
        $remaining=500-$step.ElapsedMilliseconds
        if($remaining -gt 0) { Start-Sleep -Milliseconds $remaining }
    }
} finally {
    $deadline=[DateTime]::UtcNow.AddSeconds($RequestTimeoutSeconds)
    try { Send-Buttons $neutral }
    catch { Write-Warning "Could not release scripted input: $($_.Exception.Message)" }
    finally { $portal.Client.Dispose() }
    if($StopAfter) {
        & (Join-Path $PSScriptRoot 'xbox-stop.ps1') -Console $Console -App $package.PackageFullName -TimeoutSeconds 20 -Json
        if($LASTEXITCODE){throw 'Could not verify the Xbox app stopped after automatic input.'}
    }
}
Write-Output 'Automatic input finished; all scripted buttons released.'
