<#
.SYNOPSIS
Stop only the selected app and wait for its processes to disappear. This is not graceful probe cancellation.
#>

#requires -Version 7.2
[CmdletBinding()]
param([string]$App='', [string]$Console='default', [string]$Url='', [switch]$Insecure, [ValidateRange(1,3600)][int]$TimeoutSeconds=30, [switch]$Json)
. (Join-Path $PSScriptRoot 'Xbox.Common.ps1')
Invoke-XboxCommand 'stop' $Json {
    param($context)
    if (!$App) { Stop-XboxCommand '-App is required.' }
    $context.Portal = Open-XboxPortal $Console $Url $Insecure $TimeoutSeconds
    $portal = $context.Portal; $package = Find-XboxApp $portal $App
    $before=(Invoke-XboxRequest $portal GET '/api/resourcemanager/processes').data
    if(!$before -or !$before.ContainsKey('Processes')) { Stop-XboxCommand 'Cannot inspect processes before stopping the app.' 3 }
    $knownIds=@($before.Processes | Where-Object { $_.ContainsKey('PackageFullName') -and $_.PackageFullName -eq $package.PackageFullName } | ForEach-Object { $_.ProcessId })
    $null = Invoke-XboxRequest $portal DELETE '/api/taskmanager/app' @{package=ConvertTo-XboxAppId $package.PackageFullName} $null @(200,202,204,404)
    do {
        $processes = (Invoke-XboxRequest $portal GET '/api/resourcemanager/processes').data
        if (!$processes -or !$processes.ContainsKey('Processes')) { Stop-XboxCommand 'Cannot verify stop: unexpected process-list schema.' 3 }
        # System processes often have no package identity. Track the selected app's
        # previously observed process IDs as well as its package identity.
        $running = @($processes.Processes | Where-Object { ($_.ContainsKey('PackageFullName') -and $_.PackageFullName -eq $package.PackageFullName) -or ($_.ContainsKey('ProcessId') -and $knownIds -contains $_.ProcessId) })
        if (!$running.Count) { return @{stopped_observed=$true; package=$package.PackageFullName} }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $portal.Deadline)
    Stop-XboxCommand 'The package was still running at timeout; no further stop request was sent.' 5
}
exit $script:XboxExitCode
