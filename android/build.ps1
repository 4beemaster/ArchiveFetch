param()
$ErrorActionPreference = 'Stop'
$tools = Join-Path $env:USERPROFILE '.archive-android'
if (-not $env:JAVA_HOME -and (Test-Path -LiteralPath "$tools/java")) {
    $env:JAVA_HOME = (Get-ChildItem -LiteralPath "$tools/java" -Directory | Select-Object -First 1).FullName
}
if (-not $env:ANDROID_HOME -and (Test-Path -LiteralPath "$tools/sdk")) {
    $env:ANDROID_HOME = "$tools/sdk"
}
if (-not $env:JAVA_HOME -or -not (Test-Path -LiteralPath "$env:JAVA_HOME/bin/java.exe")) {
    throw 'Install JDK 17 and set JAVA_HOME.'
}
if (-not $env:ANDROID_HOME -or -not (Test-Path -LiteralPath "$env:ANDROID_HOME/platforms/android-36/android.jar")) {
    throw 'Install Android SDK 36 and set ANDROID_HOME.'
}
Push-Location $PSScriptRoot
try {
    & .\gradlew.bat --project-cache-dir .gradle-arm64-v8a :app:assembleDebug :app:lintDebug --console=plain
    if ($LASTEXITCODE -ne 0) { throw "Android build failed ($LASTEXITCODE)." }
    $output = Join-Path $PSScriptRoot '../build'
    New-Item -ItemType Directory -Force -Path $output | Out-Null
    $apk = Join-Path $output 'ArchiveFetch-arm64-debug.apk'
    Copy-Item -LiteralPath './app/build/outputs/apk/debug/app-debug.apk' -Destination $apk -Force
    Get-Item -LiteralPath $apk | Select-Object FullName, Length
    Get-FileHash -LiteralPath $apk -Algorithm SHA256
} finally { Pop-Location }