param([Parameter(Mandatory)][string]$Output)
# Writes the Windows trusted root certificates as a PEM bundle (64-character lines, LF endings).
$ErrorActionPreference = 'Stop'
$seen = @{}
$text = New-Object System.Text.StringBuilder
$certificates = foreach ($location in 'LocalMachine', 'CurrentUser') {
    # The X509Store API, not the Cert: drive, which is missing when started from PowerShell 7.
    $store = New-Object System.Security.Cryptography.X509Certificates.X509Store('Root', $location)
    $store.Open('ReadOnly')
    $store.Certificates
    $store.Close()
}
foreach ($certificate in $certificates) {
    if ($seen.ContainsKey($certificate.Thumbprint)) { continue }
    $seen[$certificate.Thumbprint] = $true
    $base64 = [Convert]::ToBase64String($certificate.RawData)
    [void]$text.Append("-----BEGIN CERTIFICATE-----`n")
    for ($index = 0; $index -lt $base64.Length; $index += 64) {
        [void]$text.Append($base64.Substring($index, [Math]::Min(64, $base64.Length - $index))).Append("`n")
    }
    [void]$text.Append("-----END CERTIFICATE-----`n")
}
if ($seen.Count -eq 0) { throw 'No trusted root certificates found' }
[IO.File]::WriteAllText($Output, $text.ToString())
