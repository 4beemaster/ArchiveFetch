param(
    [Parameter(Mandatory)][string]$PackageDir,
    [string]$OutputDir,
    [string]$Compiler
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$version = [IO.File]::ReadAllText((Join-Path $root 'VERSION')).Trim()
if ($version -notmatch '^(\d+)\.(\d+)\.(\d+)(?:-beta\.(\d+))?$') {
    throw 'VERSION must be a release or beta version.'
}
$revision = if ($Matches[4]) { [int]$Matches[4] } else { 65535 }
$numericVersion = "$($Matches[1]).$($Matches[2]).$($Matches[3]).$revision"
$package = (Resolve-Path -LiteralPath $PackageDir).Path
if (-not $OutputDir) { $OutputDir = Join-Path $root 'build-windows-installer' }
$output = [IO.Path]::GetFullPath($OutputDir)
if (-not $Compiler) {
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 7\ISCC.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe')
    )
    $Compiler = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $Compiler -or -not (Test-Path -LiteralPath $Compiler)) {
    throw 'Install Inno Setup 6 or 7, or supply -Compiler.'
}
foreach ($file in 'archive_catalog_web.exe','LICENSE','payload\ArchiveFetch.elf','payload\ArchiveFetch.version','web\index.html') {
    if (-not (Test-Path -LiteralPath (Join-Path $package $file))) { throw "Package is missing $file" }
}
if ([IO.File]::ReadAllText((Join-Path $package 'payload\ArchiveFetch.version')).Trim() -ne $version) {
    throw 'Packaged payload does not match VERSION.'
}
$builtElf = Join-Path $root 'build-ps5\ArchiveFetch.elf'
if (-not (Test-Path -LiteralPath $builtElf) -or
    (Get-FileHash -LiteralPath $builtElf).Hash -ne (Get-FileHash -LiteralPath (Join-Path $package 'payload\ArchiveFetch.elf')).Hash) {
    throw 'Packaged payload does not match the current PS5 build.'
}
& $Compiler "/DPackageDir=$package" "/DReleaseVersion=$version" "/DVersionNumber=$numericVersion" "/DOutputDir=$output" (Join-Path $PSScriptRoot 'ArchiveFetch.iss')
if ($LASTEXITCODE -ne 0) { throw "Installer compilation failed ($LASTEXITCODE)." }
$installer = Join-Path $output "ArchiveFetch-$version-windows-x64-setup.exe"
Get-Item -LiteralPath $installer | Select-Object FullName, Length
Get-FileHash -LiteralPath $installer -Algorithm SHA256
