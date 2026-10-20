<#
.SYNOPSIS
Delete one file from a named app's LocalState, for example run.txt to start the app as players do. A missing file is not an error.
#>

#requires -Version 7.2
[CmdletBinding()]
param([string]$App='', [string]$RemotePath='', [string]$Console='default', [string]$Url='', [switch]$Insecure, [ValidateRange(1,3600)][int]$TimeoutSeconds=90, [switch]$Json)
. (Join-Path $PSScriptRoot 'Xbox.Common.ps1')
Invoke-XboxCommand 'delete' $Json {
    param($context)
    if (!$App) { Stop-XboxCommand '-App is required.' }
    $null = Get-XboxFileQuery @{PackageFullName='validation'} $RemotePath
    $context.Portal = Open-XboxPortal $Console $Url $Insecure $TimeoutSeconds
    $portal = $context.Portal; $package = Find-XboxApp $portal $App
    $query = Get-XboxFileQuery $package $RemotePath
    $listingQuery = @{knownfolderid=$query.knownfolderid; packagefullname=$query.packagefullname; path=$query.path}
    $listing = (Invoke-XboxRequest $portal GET '/api/filesystem/apps/files' $listingQuery).data
    if (!$listing -or !$listing.ContainsKey('Items')) { Stop-XboxCommand 'Cannot check the file: unexpected file-list schema.' 3 }
    if (!@($listing.Items | Where-Object { $_.Name -eq $query.filename }).Count) { return @{deleted=$false; existed=$false; package=$package.PackageFullName; remote_path=$RemotePath} }
    $null = Invoke-XboxRequest $portal DELETE '/api/filesystem/apps/file' $query $null @(200,204)
    $listing = (Invoke-XboxRequest $portal GET '/api/filesystem/apps/files' $listingQuery).data
    if (@($listing.Items | Where-Object { $_.Name -eq $query.filename }).Count) { Stop-XboxCommand 'The file is still there after the delete request.' 5 }
    @{deleted=$true; existed=$true; package=$package.PackageFullName; remote_path=$RemotePath}
}
exit $script:XboxExitCode
