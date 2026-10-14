#requires -Version 7.2
# Shared connection, encoding and file handling. Operations live in the individual scripts.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:XboxWorkspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$script:XboxOutputRoot = Join-Path $script:XboxWorkspace '.local/transfers'
$script:XboxRawRoot = Join-Path $script:XboxWorkspace '.local/results'

function Stop-XboxCommand([string]$Message, [int]$Code = 2) {
    $errorValue = [InvalidOperationException]::new($Message)
    $errorValue.Data['XboxExitCode'] = $Code
    throw $errorValue
}

function Assert-XboxPath([string]$Path) {
    $resolvedPath = [IO.Path]::GetFullPath($Path)
    if (!$resolvedPath.StartsWith($script:XboxWorkspace + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        Stop-XboxCommand 'Local files must stay inside this project directory.'
    }
    $ancestor = $resolvedPath
    while ($ancestor) {
        if ((Test-Path -LiteralPath $ancestor) -and ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            Stop-XboxCommand 'Local paths containing links are refused.'
        }
        if ($ancestor -eq $script:XboxWorkspace) { break }
        $ancestor = [IO.Path]::GetDirectoryName($ancestor)
    }
    return $resolvedPath
}

function Get-XboxInputFile([string]$Path) {
    if (!$Path) { Stop-XboxCommand 'A local input file is required.' }
    $resolvedPath = Assert-XboxPath $Path
    if (!(Test-Path -LiteralPath $resolvedPath -PathType Leaf)) { Stop-XboxCommand 'The requested input file does not exist.' }
    return $resolvedPath
}

function Get-XboxDeviceConfig([string]$Console, [string]$Url = '', [string]$DevicesPath = '') {
    if ($Console -notmatch '^[A-Za-z0-9][A-Za-z0-9_-]{0,79}$') { Stop-XboxCommand 'Invalid console configuration name.' }
    if ($Url) { return @{url=$Url; auth='none'; insecure_tls=$false} }
    if (!$DevicesPath) { $DevicesPath = Join-Path $script:XboxWorkspace '.local/devices.json' }
    $configPath = Assert-XboxPath $DevicesPath
    if (!(Test-Path -LiteralPath $configPath -PathType Leaf)) { Stop-XboxCommand 'Supply -Url or copy devices.example.json to devices.json and configure it.' }
    try { $devices = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json -AsHashtable }
    catch { Stop-XboxCommand 'Invalid devices.json JSON.' }
    if ($devices -isnot [Collections.IDictionary]) { Stop-XboxCommand 'devices.json must be an object keyed by console name.' }
    if (!$devices.Contains($Console)) { Stop-XboxCommand 'Unknown console key. Select a key from devices.json with -Console.' }
    $config = $devices[$Console]
    if ($config -isnot [Collections.IDictionary] -or !$config.Contains('url') -or $config.url -isnot [string]) { Stop-XboxCommand 'The selected console must have a URL string.' }
    if ($config.Contains('insecure_tls') -and $config.insecure_tls -isnot [bool]) { Stop-XboxCommand 'insecure_tls must be true or false, not a string.' }
    return $config
}

function Open-XboxPortal([string]$Console, [string]$Url, [bool]$Insecure, [int]$TimeoutSeconds) {
    $config = Get-XboxDeviceConfig $Console $Url
    $privateRoot = Join-Path $script:XboxWorkspace '.local'
    $uri = $null
    if (![uri]::TryCreate([string]$config.url, [UriKind]::Absolute, [ref]$uri) -or $uri.Scheme -notin @('http','https') -or $uri.UserInfo -or $uri.Query -or $uri.Fragment -or $uri.AbsolutePath -ne '/') {
        Stop-XboxCommand 'Use an HTTP(S) portal origin without credentials, query, fragment or path.'
    }
    $handler = [Net.Http.HttpClientHandler]::new()
    $handler.AllowAutoRedirect = $false
    $handler.CookieContainer = [Net.CookieContainer]::new()
    if ($Insecure -or ($config.ContainsKey('insecure_tls') -and $config.insecure_tls)) {
        $handler.ServerCertificateCustomValidationCallback = [Net.Http.HttpClientHandler]::DangerousAcceptAnyServerCertificateValidator
    }
    $client = [Net.Http.HttpClient]::new($handler)
    $client.BaseAddress = $uri
    $client.Timeout = [Threading.Timeout]::InfiniteTimeSpan
    try {
        $auth = if ($config.ContainsKey('auth')) { [string]$config.auth } else { 'none' }
        if ($auth -ne 'none') {
            if ($uri.Scheme -ne 'https') { Stop-XboxCommand 'Authenticated connections require HTTPS.' }
            if ($auth -eq 'environment') {
                $usernameName = if ($config.ContainsKey('username_env')) { $config.username_env } else { 'XBOX_USERNAME' }
                $passwordName = if ($config.ContainsKey('password_env')) { $config.password_env } else { 'XBOX_PASSWORD' }
                $usernameValue = [Environment]::GetEnvironmentVariable($usernameName)
                $passwordValue = [Environment]::GetEnvironmentVariable($passwordName)
                if (!$usernameValue -or !$passwordValue) { Stop-XboxCommand 'Configured credential environment variables are not set.' }
            } elseif ($auth -eq 'credential') {
                if (!$IsWindows -or !$config.ContainsKey('credential_file') -or $config.credential_file -notmatch '^[A-Za-z0-9_-]+\.credential\.xml$') { Stop-XboxCommand 'A Windows DPAPI credential filename is required.' }
                $credentialPath = Assert-XboxPath (Join-Path $privateRoot $config.credential_file)
                try { $credentialValue = Import-Clixml -LiteralPath $credentialPath }
                catch { Stop-XboxCommand 'Cannot load the configured Windows credential.' }
                if ($credentialValue -isnot [pscredential]) { Stop-XboxCommand 'Invalid Windows credential file.' }
                $usernameValue = $credentialValue.UserName
                $passwordValue = $credentialValue.GetNetworkCredential().Password
            } else { Stop-XboxCommand 'Unknown authentication mode.' }
            $encoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($usernameValue + ':' + $passwordValue))
            $client.DefaultRequestHeaders.Authorization = [Net.Http.Headers.AuthenticationHeaderValue]::new('Basic', $encoded)
            $usernameValue = $null; $passwordValue = $null; $credentialValue = $null; $encoded = $null
        }
        $portal = @{Client=$client; Handler=$handler; Deadline=[DateTime]::UtcNow.AddSeconds($TimeoutSeconds); PackageBase=$null}
        $null = Invoke-XboxRequest $portal GET '/'
        return $portal
    } catch { $client.Dispose(); throw }
}

function Invoke-XboxRequest($Portal, [string]$Method, [string]$Path, $Query = @{}, $Content = $null, [int[]]$Allowed = @(200), [string]$OutFile = '', [long]$MaxBytes = 67108864) {
    $remaining = $Portal.Deadline - [DateTime]::UtcNow
    if ($remaining.TotalMilliseconds -le 0) { Stop-XboxCommand 'Operation timed out; no automatic retry or cleanup was performed.' 5 }
    $queryParts = foreach ($key in $Query.Keys) { [uri]::EscapeDataString([string]$key) + '=' + [uri]::EscapeDataString([string]$Query[$key]) }
    $relative = $Path + $(if ($queryParts) { '?' + ($queryParts -join '&') } else { '' })
    $request = [Net.Http.HttpRequestMessage]::new([Net.Http.HttpMethod]::new($Method), $relative)
    if ($Content) { $request.Content = $Content }
    $csrf = $Portal.Handler.CookieContainer.GetCookies($Portal.Client.BaseAddress)['CSRF-Token']
    if ($csrf) { [void]$request.Headers.TryAddWithoutValidation('X-CSRF-Token', [uri]::UnescapeDataString($csrf.Value)) }
    $timer = [Threading.CancellationTokenSource]::new($remaining)
    $response = $null
    try {
        try { $response = $Portal.Client.SendAsync($request, [Net.Http.HttpCompletionOption]::ResponseHeadersRead, $timer.Token).GetAwaiter().GetResult() }
        catch {
            if ($timer.IsCancellationRequested) { Stop-XboxCommand 'Operation timed out; partial evidence was retained.' 5 }
            Stop-XboxCommand 'Portal connection failed. Check reachability, authentication and TLS configuration.' 3
        }
        $status = [int]$response.StatusCode
        if ($status -notin $Allowed) { Stop-XboxCommand ("Portal returned HTTP $status for $Method $Path.") 3 }
        if ($OutFile) {
            $destination = Assert-XboxPath $OutFile
            if ((Test-Path -LiteralPath $destination) -or (Test-Path -LiteralPath ($destination + '.partial'))) { Stop-XboxCommand 'Destination already exists; overwriting is refused.' }
            if ($response.Content.Headers.ContentLength -gt $MaxBytes) { Stop-XboxCommand 'Download exceeds the configured size limit.' 4 }
            $stream = $response.Content.ReadAsStreamAsync($timer.Token).GetAwaiter().GetResult()
            $file = [IO.File]::Open($destination + '.partial', [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
            try {
                $buffer = [byte[]]::new(65536); [long]$total = 0
                while (($count = $stream.ReadAsync($buffer, 0, $buffer.Length, $timer.Token).GetAwaiter().GetResult()) -gt 0) {
                    $total += $count
                    if ($total -gt $MaxBytes) { Stop-XboxCommand 'Download exceeds the configured size limit; partial data was retained.' 4 }
                    $file.Write($buffer, 0, $count)
                }
            } finally { $file.Dispose(); $stream.Dispose() }
            [IO.File]::Move($destination + '.partial', $destination, $false)
            return @{status=$status; file=$destination; bytes=$total}
        }
        $body = $response.Content.ReadAsStringAsync($timer.Token).GetAwaiter().GetResult()
        $data = $null
        if ($body.TrimStart().StartsWith('{') -or $body.TrimStart().StartsWith('[')) {
            try { $data = ConvertFrom-Json -InputObject $body -AsHashtable }
            catch { Stop-XboxCommand 'Portal returned invalid JSON.' 3 }
        }
        return @{status=$status; data=$data}
    } catch {
        if ($timer.IsCancellationRequested) { Stop-XboxCommand 'Operation timed out; partial evidence was retained.' 5 }
        throw
    } finally { if ($response) { $response.Dispose() }; $request.Dispose(); $timer.Dispose() }
}

function Get-XboxPackages($Portal) {
    foreach ($base in $(if ($Portal.PackageBase) { @($Portal.PackageBase) } else { @('/api/app/packagemanager','/api/appx/packagemanager') })) {
        $result = Invoke-XboxRequest $Portal GET ($base + '/packages') @{} $null @(200,404)
        if ($result.status -eq 200) {
            if (!$result.data -or !$result.data.ContainsKey('InstalledPackages')) { Stop-XboxCommand 'Unexpected package-list schema.' 3 }
            $Portal.PackageBase = $base
            return ,@($result.data.InstalledPackages)
        }
    }
    Stop-XboxCommand 'No supported package-manager endpoint was found.' 3
}

function Find-XboxApp($Portal, [string]$App) {
    if (!$App) { Stop-XboxCommand '-App is required.' }
    $packages = Get-XboxPackages $Portal
    $found = @($packages | Where-Object { $_.Name -eq $App -or $_.PackageFullName -eq $App -or $_.PackageFamilyName -eq $App -or ([string]$_.PackageFullName).Split('_')[0] -eq $App })
    if ($found.Count -ne 1) { Stop-XboxCommand 'App selection must match exactly one installed package. Use its PackageFullName.' }
    return $found[0]
}

function ConvertTo-XboxAppId([string]$Value) { return [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Value)) }

function Get-XboxFileQuery($Package, [string]$RemotePath, [switch]$Folder) {
    $remote = $RemotePath.Replace('/','\').TrimStart('\')
    if ($remote -notmatch '^LocalState(\\|$)' -or $remote -match '(^|\\)\.\.?($|\\)|[:\r\n]|[<>"|?*]' -or $remote.Contains('\\')) { Stop-XboxCommand 'Use a relative LocalState path without traversal or special characters.' }
    $query = @{knownfolderid='LocalAppData'; packagefullname=$Package.PackageFullName; path='\' + $remote}
    if (!$Folder) {
        $split = $remote.LastIndexOf('\')
        if ($split -lt 1 -or $split -eq $remote.Length - 1) { Stop-XboxCommand 'Specify a filename below LocalState.' }
        $query.path = '\' + $remote.Substring(0,$split); $query.filename = $remote.Substring($split + 1)
    }
    return $query
}

function New-XboxMultipart([string[]]$Paths, [string[]]$Names = @()) {
    $form = [Net.Http.MultipartFormDataContent]::new()
    try {
        for ($index = 0; $index -lt $Paths.Count; $index++) {
            $path = Get-XboxInputFile $Paths[$index]
            $name = if ($Names.Count -gt $index) { $Names[$index] } else { [IO.Path]::GetFileName($path) }
            $part = [Net.Http.StreamContent]::new([IO.File]::OpenRead($path))
            $part.Headers.ContentType = [Net.Http.Headers.MediaTypeHeaderValue]::new('application/octet-stream')
            $form.Add($part, $name, $name)
            $part.Headers.ContentDisposition.Name = '"' + $name + '"'
            $part.Headers.ContentDisposition.FileName = '"' + $name + '"'
            $part.Headers.ContentDisposition.FileNameStar = $null
        }
        return ,$form
    } catch { $form.Dispose(); throw }
}

function New-XboxOutput($Context, [string]$Operation, [string]$FileName, $Request) {
    $id = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '_' + $Operation + '_' + [guid]::NewGuid().ToString('N').Substring(0,6)
    $directory = Assert-XboxPath (Join-Path $script:XboxOutputRoot $id)
    [void][IO.Directory]::CreateDirectory($directory)
    $Context.Output = $directory
    $Context.ActivePath = Join-Path $directory '.active'
    $Context.ActiveLease = [IO.File]::Open($Context.ActivePath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    [IO.File]::WriteAllText((Join-Path $directory 'request.json'), ($Request | ConvertTo-Json -Depth 8) + "`n", [Text.UTF8Encoding]::new($false))
    [Console]::Error.WriteLine('Output: scripts/output/' + $id)
    return Join-Path $directory $FileName
}

function Get-XboxProbeRun([string]$Probe, [string]$RunId) {
    foreach ($segment in @($Probe, $RunId)) {
        if ($segment -notmatch '\A[A-Za-z0-9][A-Za-z0-9._-]{0,127}\z' -or $segment.EndsWith('.') -or $segment -match '\A(?i:CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\.|$)') {
            Stop-XboxCommand '-Probe and -RunId must be safe single folder names, at most 128 characters.'
        }
    }
    return Assert-XboxPath (Join-Path (Join-Path $script:XboxRawRoot $Probe) $RunId)
}

function New-XboxProbeOutput($Context, [string]$RunDirectory, [string]$RelativeFile, $Request) {
    $dataDirectory = Assert-XboxPath (Join-Path $RunDirectory 'data')
    $destination = Assert-XboxPath (Join-Path $dataDirectory $RelativeFile)
    if ((Test-Path -LiteralPath $destination) -or (Test-Path -LiteralPath ($destination + '.partial'))) {
        Stop-XboxCommand 'Destination already exists; overwriting is refused.'
    }
    $id = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '_download_' + [guid]::NewGuid().ToString('N').Substring(0,6)
    $receiptDirectory = Assert-XboxPath (Join-Path (Join-Path $RunDirectory 'collection') $id)
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
    [void][IO.Directory]::CreateDirectory($receiptDirectory)
    $Context.Output = $receiptDirectory
    $Context.ActivePath = Join-Path $receiptDirectory '.active'
    $Context.ActiveLease = [IO.File]::Open($Context.ActivePath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    [IO.File]::WriteAllText((Join-Path $receiptDirectory 'request.json'), ($Request | ConvertTo-Json -Depth 8) + "`n", [Text.UTF8Encoding]::new($false))
    [Console]::Error.WriteLine('Output: ' + [IO.Path]::GetRelativePath($script:XboxWorkspace, $RunDirectory))
    return $destination
}

function Invoke-XboxCommand([string]$Operation, [bool]$Json, [scriptblock]$Action) {
    $context = @{Portal=$null; Output=$null; ActivePath=$null; ActiveLease=$null}
    $script:XboxExitCode = 0
    try {
        $result = & $Action $context
        if ($script:XboxExitCode) {
            $envelope = @{schema_version=1; ok=$false; operation=$Operation; data=$result; error=@{code=$script:XboxExitCode; message='Operation did not complete. Review the partial result; remaining files were retained.'}}
        } else { $envelope = @{schema_version=1; ok=$true; operation=$Operation; data=$result} }
    } catch {
        $script:XboxExitCode = if ($_.Exception.Data.Contains('XboxExitCode')) { [int]$_.Exception.Data['XboxExitCode'] } else { 1 }
        $message = if ($_.Exception.Data.Contains('XboxExitCode')) { $_.Exception.Message } else { 'Unexpected local or response error. No automatic retry or cleanup was performed.' }
        $envelope = @{schema_version=1; ok=$false; operation=$Operation; error=@{code=$script:XboxExitCode; message=$message}}
    } finally { if ($context.Portal) { $context.Portal.Client.Dispose() } }
    if ($context.Output) {
        try { [IO.File]::WriteAllText((Join-Path $context.Output 'result.json'), ($envelope | ConvertTo-Json -Depth 15) + "`n", [Text.UTF8Encoding]::new($false)) }
        catch { $script:XboxExitCode = 1; $envelope = @{schema_version=1; ok=$false; operation=$Operation; error=@{code=1; message='Cannot save the local result record.'}} }
    }
    if ($context.ActiveLease) {
        $context.ActiveLease.Dispose()
        try { [IO.File]::Delete($context.ActivePath) } catch { } # A stale unlocked marker is harmless.
    }
    if ($Json) { $envelope | ConvertTo-Json -Depth 20 }
    elseif (!$envelope.ok) {
        [Console]::Error.WriteLine($envelope.error.message)
        if ($envelope.ContainsKey('data')) { $envelope.data | ConvertTo-Json -Depth 20 }
    }
    else { $envelope.data | ConvertTo-Json -Depth 20 }
}
