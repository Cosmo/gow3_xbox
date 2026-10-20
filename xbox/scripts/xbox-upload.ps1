<#
.SYNOPSIS
Upload one file to a named app's LocalState. Existing remote files require -Overwrite.
#>

#requires -Version 7.2
[CmdletBinding()]
param([string]$App='', [string]$File='', [string]$RemotePath='', [switch]$Overwrite, [string]$Console='default', [string]$Url='', [switch]$Insecure, [ValidateRange(1,3600)][int]$TimeoutSeconds=90, [switch]$Json)
. (Join-Path $PSScriptRoot 'Xbox.Common.ps1')
Invoke-XboxCommand 'upload' $Json {
    param($context)
    if (!$App) { Stop-XboxCommand '-App is required.' }
    $localFile = Get-XboxInputFile $File
    $null = Get-XboxFileQuery @{PackageFullName='validation'} $RemotePath
    $context.Portal = Open-XboxPortal $Console $Url $Insecure $TimeoutSeconds
    $portal = $context.Portal; $package = Find-XboxApp $portal $App
    $query = Get-XboxFileQuery $package $RemotePath
    $listingQuery = @{knownfolderid=$query.knownfolderid; packagefullname=$query.packagefullname; path=$query.path}
    $listing = (Invoke-XboxRequest $portal GET '/api/filesystem/apps/files' $listingQuery).data
    if (!$listing -or !$listing.ContainsKey('Items')) { Stop-XboxCommand 'Cannot check destination: unexpected file-list schema.' 3 }
    if (!$Overwrite -and @($listing.Items | Where-Object { $_.Name -eq $query.filename }).Count) { Stop-XboxCommand 'Remote file exists; use -Overwrite only when replacement is intended.' }
    $query.extract = 'false'
    $form = New-XboxMultipart @($localFile) @($query.filename)
    $null = Invoke-XboxRequest $portal POST '/api/filesystem/apps/file' $query $form @(200,201,204)
    @{uploaded=$true; package=$package.PackageFullName; remote_path=$RemotePath}
}
exit $script:XboxExitCode
