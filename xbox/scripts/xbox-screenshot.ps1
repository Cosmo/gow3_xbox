<#
.SYNOPSIS
Capture one Xbox screenshot into a new ignored scripts/output session.
#>

#requires -Version 7.2
[CmdletBinding()]
param([string]$Console='default', [string]$Url='', [switch]$Insecure, [ValidateRange(1,3600)][int]$TimeoutSeconds=30, [switch]$Json)
. (Join-Path $PSScriptRoot 'Xbox.Common.ps1')
Invoke-XboxCommand 'screenshot' $Json {
    param($context)
    $context.Portal = Open-XboxPortal $Console $Url $Insecure $TimeoutSeconds
    $destination = New-XboxOutput $context 'screenshot' 'screenshot.png' @{format='png'; capture_count=1}
    $result = Invoke-XboxRequest $context.Portal GET '/ext/screenshot' @{} $null @(200) $destination
    $file = [IO.File]::OpenRead($destination)
    try { $signature = [byte[]]::new(8); $count = $file.Read($signature,0,8) } finally { $file.Dispose() }
    if ($count -ne 8 -or [Convert]::ToHexString($signature) -ne '89504E470D0A1A0A') { Stop-XboxCommand 'Screenshot response is not a PNG; received evidence was retained.' 4 }
    @{file=$result.file; bytes=$result.bytes; format='png'}
}
exit $script:XboxExitCode
