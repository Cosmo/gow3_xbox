<#
.SYNOPSIS
Launch the selected app normally and wait for its package to appear in the process list. Some probes start automatically.
#>

#requires -Version 7.2
[CmdletBinding()]
param([string]$App='', [string]$Console='default', [string]$Url='', [switch]$Insecure, [ValidateRange(1,3600)][int]$TimeoutSeconds=30, [switch]$Json)
. (Join-Path $PSScriptRoot 'Xbox.Common.ps1')
Invoke-XboxCommand 'launch' $Json {
    param($context)
    if (!$App) { Stop-XboxCommand '-App is required.' }
    $context.Portal = Open-XboxPortal $Console $Url $Insecure $TimeoutSeconds
    $portal = $context.Portal; $package = Find-XboxApp $portal $App
    if (!$package.PackageRelativeId) { Stop-XboxCommand 'Package has no launchable application ID.' 4 }
    $null = Invoke-XboxRequest $portal POST '/api/taskmanager/app' @{appid=ConvertTo-XboxAppId $package.PackageRelativeId; package=ConvertTo-XboxAppId $package.PackageFullName} $null @(200,202,204)
    do {
        $processes = (Invoke-XboxRequest $portal GET '/api/resourcemanager/processes').data
        if (!$processes -or !$processes.ContainsKey('Processes')) { Stop-XboxCommand 'Cannot verify launch: unexpected process-list schema.' 3 }
        $running = @($processes.Processes | Where-Object { $_.ContainsKey('PackageFullName') -and $_.PackageFullName -eq $package.PackageFullName })
        if ($running.Count) { return @{running_observed=$true; package=$package.PackageFullName; process_ids=@($running | ForEach-Object { $_.ProcessId })} }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $portal.Deadline)
    Stop-XboxCommand 'Launch was accepted but the package was not observed running before timeout. It was not stopped.' 5
}
exit $script:XboxExitCode
