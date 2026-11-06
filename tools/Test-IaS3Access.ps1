param(
    [Parameter(Mandatory = $true)]
    [uri] $DownloadUrl
)

$ErrorActionPreference = 'Stop'

function Assert-ArchiveUrl([uri] $Url) {
    if (-not $Url.IsAbsoluteUri -or $Url.Scheme -ne 'https' -or
        $Url.Port -ne 443 -or $Url.UserInfo -or
        ($Url.DnsSafeHost -ne 'archive.org' -and
         -not $Url.DnsSafeHost.EndsWith('.archive.org', [StringComparison]::OrdinalIgnoreCase))) {
        throw 'Only HTTPS archive.org URLs on port 443 are allowed.'
    }
}

function Test-Download([string] $Label, [string] $Authorization) {
    $currentUrl = $DownloadUrl
    for ($redirect = 0; $redirect -le 8; $redirect++) {
        Assert-ArchiveUrl $currentUrl
        $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Get, $currentUrl)
        $request.Headers.Range = [System.Net.Http.Headers.RangeHeaderValue]::new(0, 4095)
        $request.Headers.UserAgent.ParseAdd('ArchiveCatalog-AuthProbe/1.0')
        if ($Authorization) {
            $request.Headers.Authorization = [System.Net.Http.Headers.AuthenticationHeaderValue]::new('LOW', $Authorization)
        }
        $response = $null
        try {
            $response = $client.SendAsync($request, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
            $status = [int] $response.StatusCode
            if ($status -in 301, 302, 303, 307, 308) {
                if (-not $response.Headers.Location) {
                    throw 'Redirect response has no destination.'
                }
                $currentUrl = [uri]::new($currentUrl, $response.Headers.Location)
                continue
            }
            $buffer = [byte[]]::new(4096)
            $totalRead = 0
            $stream = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
            $cancellation = [System.Threading.CancellationTokenSource]::new([TimeSpan]::FromSeconds(30))
            try {
                while ($totalRead -lt $buffer.Length) {
                    $bytesRead = $stream.ReadAsync($buffer, $totalRead, $buffer.Length - $totalRead, $cancellation.Token).GetAwaiter().GetResult()
                    if ($bytesRead -eq 0) { break }
                    $totalRead += $bytesRead
                }
            }
            finally {
                $stream.Dispose()
                $cancellation.Dispose()
            }
            $hasChdHeader = $totalRead -ge 8 -and [Text.Encoding]::ASCII.GetString($buffer, 0, 8) -ceq 'MComprHD'
            [pscustomobject]@{
                Test = $Label
                Status = $status
                ContentType = [string] $response.Content.Headers.ContentType
                ContentRange = [string] $response.Content.Headers.ContentRange
                BytesRead = $totalRead
                ChdHeader = $hasChdHeader
                FileAccess = ($status -in 200, 206) -and $hasChdHeader
            }
            return
        }
        finally {
            if ($response) { $response.Dispose() }
            $request.Dispose()
        }
    }
    throw 'Too many redirects.'
}

Assert-ArchiveUrl $DownloadUrl
if ($DownloadUrl.AbsolutePath -notlike '/download/*/*.chd' -or $DownloadUrl.Query -or $DownloadUrl.Fragment) {
    throw 'Provide a direct https://archive.org/download/ITEM/FILE.chd URL without query parameters.'
}

Add-Type -AssemblyName System.Net.Http
$handler = [System.Net.Http.HttpClientHandler]::new()
$handler.AllowAutoRedirect = $false
$handler.UseCookies = $false
$client = [System.Net.Http.HttpClient]::new($handler)
$client.Timeout = [TimeSpan]::FromSeconds(30)
$credential = $null
$authorization = $null
try {
    Write-Host 'No browser cookies are used. Keys stay in this process and are not saved.'
    $anonymous = Test-Download 'Anonymous' ''
    $anonymous | Format-Table -AutoSize
    $accessKey = Read-Host 'IA-S3 access key' -AsSecureString
    $secretKey = Read-Host 'IA-S3 secret key' -AsSecureString
    $credential = [System.Net.NetworkCredential]::new('', $accessKey)
    $accessText = $credential.Password
    $credential = [System.Net.NetworkCredential]::new('', $secretKey)
    $secretText = $credential.Password
    if (-not $accessText -or -not $secretText -or $accessText -match '[:\s]' -or $secretText -match '\s') {
        throw 'Keys are empty or contain unexpected characters.'
    }
    $authorization = $accessText + ':' + $secretText
    $authenticated = Test-Download 'IA-S3 only' $authorization
    $authenticated | Format-Table -AutoSize
    if ($anonymous.FileAccess) {
        Write-Host 'Inconclusive: this URL was also accessible anonymously. Choose a known restricted CHD.'
    }
    elseif ($authenticated.FileAccess) {
        Write-Host 'S3-only access succeeded for this file. This does not establish access to every restricted item.'
    }
    else {
        Write-Host 'S3-only access was not demonstrated. Check key validity and account permission for this same URL.'
    }
}
catch {
    Write-Error 'Probe failed (network, URL, redirect, or credential error). Details suppressed to avoid exposing credentials.'
}
finally {
    $authorization = $null
    $accessText = $null
    $secretText = $null
    $credential = $null
    if ($accessKey) { $accessKey.Dispose() }
    if ($secretKey) { $secretKey.Dispose() }
    $client.Dispose()
}