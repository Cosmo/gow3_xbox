<#
.SYNOPSIS
Install one existing signed .appx and explicitly supplied dependencies. Does not build, sign, uninstall or launch.
#>

#requires -Version 7.2
[CmdletBinding()]
param([string]$Package='', [string[]]$Dependency=@(), [string]$Console='default', [string]$Url='', [switch]$Insecure, [ValidateRange(1,3600)][int]$TimeoutSeconds=180, [switch]$Json)
. (Join-Path $PSScriptRoot 'Xbox.Common.ps1')
Invoke-XboxCommand 'install' $Json {
    param($context)
    $packageFile = Get-XboxInputFile $Package
    if ([IO.Path]::GetExtension($packageFile) -ne '.appx') { Stop-XboxCommand 'This version accepts a signed .appx, not bundles or loose layouts.' }
    foreach ($dependencyFile in $Dependency) { $null = Get-XboxInputFile $dependencyFile }
    $archive = [IO.Compression.ZipFile]::OpenRead($packageFile)
    try {
        $entry = $archive.GetEntry('AppxManifest.xml')
        if (!$entry) { Stop-XboxCommand 'Package has no AppxManifest.xml.' }
        $reader = [IO.StreamReader]::new($entry.Open())
        try { [xml]$manifest = $reader.ReadToEnd() } finally { $reader.Dispose() }
        $identity = [string]$manifest.Package.Identity.Name; $version = [string]$manifest.Package.Identity.Version
        if (!$identity -or !$version) { Stop-XboxCommand 'Package identity or version is missing.' }
    } finally { $archive.Dispose() }
    $context.Portal = Open-XboxPortal $Console $Url $Insecure $TimeoutSeconds
    $portal = $context.Portal
    $null = Get-XboxPackages $portal
    [Console]::Error.WriteLine('Uploading package; waiting for installation completion...')
    $form = New-XboxMultipart (@($packageFile) + $Dependency)
    $null = Invoke-XboxRequest $portal POST ($portal.PackageBase + '/package') @{package=[IO.Path]::GetFileName($packageFile)} $form @(200,202,204)
    do {
        $state = Invoke-XboxRequest $portal GET ($portal.PackageBase + '/state') @{} $null @(200,204)
        if ($state.status -eq 200) {
            if (!$state.data -or !$state.data.ContainsKey('Success') -or !$state.data.ContainsKey('Code')) { Stop-XboxCommand 'Unexpected installation-state schema.' 3 }
            if (!$state.data.Success -or [long]$state.data.Code -ne 0) { Stop-XboxCommand 'Portal reported installation failure.' 4 }
            $packages = Get-XboxPackages $portal
            $installed = @($packages | Where-Object {
                $actualVersion = if ($_.Version -is [string]) { $_.Version } else { '{0}.{1}.{2}.{3}' -f $_.Version.Major,$_.Version.Minor,$_.Version.Build,$_.Version.Revision }
                ([string]$_.PackageFullName).Split('_')[0] -eq $identity -and $actualVersion -eq $version
            })
            if ($installed.Count -eq 1) { return @{installed=$true; package=$installed[0].PackageFullName; version=$version} }
        }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $portal.Deadline)
    Stop-XboxCommand 'Installation did not complete with the exact package/version before timeout.' 5
}
exit $script:XboxExitCode
