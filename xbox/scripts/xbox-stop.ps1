<#
.SYNOPSIS
Stop only the selected app and verify it stops running. Xbox may retain an inactive process record.
#>

#requires -Version 7.2
[CmdletBinding()]
param([string]$App='', [string]$Console='default', [string]$Url='', [switch]$Insecure, [ValidateRange(1,3600)][int]$TimeoutSeconds=120, [switch]$Json)
. (Join-Path $PSScriptRoot 'Xbox.Common.ps1')
Invoke-XboxCommand 'stop' $Json {
    param($context)
    if (!$App) { Stop-XboxCommand '-App is required.' }
    $context.Portal = Open-XboxPortal $Console $Url $Insecure $TimeoutSeconds
    $portal = $context.Portal; $package = Find-XboxApp $portal $App
    $before=(Invoke-XboxRequest $portal GET '/api/resourcemanager/processes').data
    if(!$before -or !$before.ContainsKey('Processes')) { Stop-XboxCommand 'Cannot inspect processes before stopping the app.' 3 }
    $knownIds=@($before.Processes | Where-Object { $_.ContainsKey('PackageFullName') -and $_.PackageFullName -eq $package.PackageFullName } | ForEach-Object { $_.ProcessId })
    foreach($processId in $knownIds) {
        $current=(Invoke-XboxRequest $portal GET '/api/resourcemanager/processes').data
        $selected=@($current.Processes | Where-Object { $_.ProcessId -eq $processId -and $_.PackageFullName -eq $package.PackageFullName })
        if($selected.Count -eq 1 -and (!$selected[0].ContainsKey('IsRunning') -or $selected[0].IsRunning)) {
            $null=Invoke-XboxRequest $portal DELETE '/api/taskmanager/process' @{pid=[string]$processId} $null @(200,202,204,404)
        }
    }
    do {
        $processes = (Invoke-XboxRequest $portal GET '/api/resourcemanager/processes').data
        if (!$processes -or !$processes.ContainsKey('Processes')) { Stop-XboxCommand 'Cannot verify stop: unexpected process-list schema.' 3 }
        # System processes often have no package identity. Track the selected app's
        # previously observed process IDs as well as its package identity.
        $selected = @($processes.Processes | Where-Object { ($_.ContainsKey('PackageFullName') -and $_.PackageFullName -eq $package.PackageFullName) -or ($_.ContainsKey('ProcessId') -and $knownIds -contains $_.ProcessId) })
        $running = @($selected | Where-Object { !$_.ContainsKey('IsRunning') -or $_.IsRunning })
        if (!$running.Count) { return @{stopped_observed=$true; package=$package.PackageFullName; inactive_process_ids=@($selected | ForEach-Object ProcessId)} }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $portal.Deadline)
    Stop-XboxCommand 'The package was still running at timeout; no further stop request was sent.' 5
}
exit $script:XboxExitCode
