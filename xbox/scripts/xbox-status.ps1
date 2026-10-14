<#
.SYNOPSIS
Read console OS information. Does not save files or change settings.
#>

#requires -Version 7.2
[CmdletBinding()]
param([string]$Console='default', [string]$Url='', [switch]$Insecure, [ValidateRange(1,3600)][int]$TimeoutSeconds=30, [switch]$Json)
. (Join-Path $PSScriptRoot 'Xbox.Common.ps1')
Invoke-XboxCommand 'status' $Json {
    param($context)
    $context.Portal = Open-XboxPortal $Console $Url $Insecure $TimeoutSeconds
    (Invoke-XboxRequest $context.Portal GET '/api/os/info').data
}
exit $script:XboxExitCode
