<#
.SYNOPSIS
Download one LocalState file. Use -Probe, -RunId and -RunPath to retain it under results/raw; otherwise use disposable scripts/output.
#>

#requires -Version 7.2
[CmdletBinding()]
param([string]$App='', [string]$RemotePath='', [string]$Probe='', [string]$RunId='', [string]$RunPath='', [ValidateRange(1,4096)][int]$MaxMiB=64, [string]$Console='default', [string]$Url='', [switch]$Insecure, [ValidateRange(1,3600)][int]$TimeoutSeconds=90, [switch]$Json)
. (Join-Path $PSScriptRoot 'Xbox.Common.ps1')
Invoke-XboxCommand 'download' $Json {
    param($context)
    if (!$App) { Stop-XboxCommand '-App is required.' }
    $validation = Get-XboxFileQuery @{PackageFullName='validation'} $RemotePath
    $runDirectory = $null
    $relativeFile = $null
    if ($Probe -or $RunId -or $RunPath) {
        $runDirectory = Get-XboxProbeRun $Probe $RunId
        if (!$RunPath) { Stop-XboxCommand '-RunPath is required for retained probe downloads: the remote file path relative to the original run.' }
        $null = Get-XboxFileQuery @{PackageFullName='validation'} ('LocalState/' + $RunPath)
        if ($RunPath.Replace('\','/').Split('/')[-1] -cne $validation.filename) { Stop-XboxCommand '-RunPath must keep the original remote filename.' }
        $relativeFile = $RunPath.Replace('\', [IO.Path]::DirectorySeparatorChar).Replace('/', [IO.Path]::DirectorySeparatorChar)
        $destination = Assert-XboxPath (Join-Path (Join-Path $runDirectory 'data') $relativeFile)
        if ((Test-Path -LiteralPath $destination) -or (Test-Path -LiteralPath ($destination + '.partial'))) { Stop-XboxCommand 'Destination already exists; overwriting is refused.' }
    }
    $context.Portal = Open-XboxPortal $Console $Url $Insecure $TimeoutSeconds
    $package = Find-XboxApp $context.Portal $App
    $request = @{app=$package.PackageFullName; remote_path=$RemotePath; max_mib=$MaxMiB}
    if ($runDirectory) {
        $request.probe = $Probe; $request.run_id = $RunId; $request.run_path = $RunPath
        $destination = New-XboxProbeOutput $context $runDirectory $relativeFile $request
    } else {
        $destination = New-XboxOutput $context 'download' ('file-' + $validation.filename) $request
    }
    $result = Invoke-XboxRequest $context.Portal GET '/api/filesystem/apps/file' (Get-XboxFileQuery $package $RemotePath) $null @(200) $destination ([long]$MaxMiB * 1048576)
    @{file=$result.file; bytes=$result.bytes; remote_files_retained=$true}
}
exit $script:XboxExitCode
