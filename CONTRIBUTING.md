# Building and contributing to ArchiveFetch

For installation, features and everyday use, see the [README](README.md). This guide covers source builds, command-line use and development checks.

## Before making changes

- Keep changes focused and include regression coverage for behavior changes.
- Preserve download verification, overwrite confirmation, credential handling and platform storage protections.
- Do not commit credentials, pairing keys, private logs, machine-specific paths or generated builds.
- Report bugs with the platform, app version, reproduction steps and exact error.
- Use only content you are authorized to download when testing.

## Versioning

[VERSION](VERSION) is the shared Windows, Android and PS5 version. Supported values are `major.minor.patch` and `major.minor.patch-beta.N`; every platform reads this file.

Android derives its version code as `major * 100000 + minor * 1000 + patch * 100 + revision`, where revision is the beta number or 99 for a stable release. For example, 1.0.1-beta.3 is 100103 and 1.0.1 is 100199.

Release notes should describe user-facing features and fixes, not build or validation history. Beta notes are cumulative within their upcoming release series; final notes consolidate that series without beta housekeeping.

## Desktop build

Requires CMake 3.20+, a C++17 compiler, libxml2, OpenSSL and curl 7.85+. Windows with MinGW is supported. The following example also requires Ninja:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUNDLED_CURL=ON
cmake --build build
ctest --test-dir build --output-on-failure
.\build\archive_catalog_web.exe
```

The Windows HTML interface requires Microsoft Edge WebView2 Runtime. Keep the generated `web` directory beside the executable, including fonts and license notices. Source-build runtime dependencies must be available on `PATH`; release installers bundle them.

Build options:

| Option | Purpose |
|---|---|
| `BUNDLED_CURL=ON` | Build pinned curl 8.18.0 rather than use an installed development package. |
| `BUILD_WEB=OFF` | Omit the HTML host and its tests; retain the command-line client. |
| `BUILD_APP=OFF` | Omit the network client and frontends; libxml2 is still required. |
| `BUILD_EXTRACTION=OFF` | Omit the shared archive engine and its tests. |

Parser-only offline builds need both `BUILD_APP=OFF` and `BUILD_EXTRACTION=OFF` to avoid those dependency downloads.

HTML builds use pinned cpp-httplib, nlohmann/json, Karla, Font Awesome Free and qrcode-generator assets; Windows also uses webview and the WebView2 SDK. Extraction builds use pinned libarchive, zlib, bzip2, liblzma and Zstd sources. Build downloads use SHA-256 verification; extraction does not fetch tools or invoke external unpackers at runtime.

Bundled desktop curl uses OpenSSL and the Windows trust store on Windows. If a build machine uses a private certificate authority, configure `CMAKE_TLS_CAINFO` instead of disabling TLS verification.

### Isolated HTML-host development

Run the Windows host without its native window:

```powershell
.\build\archive_catalog_web.exe --no-browser --port 48931 --data-dir .\build\dev-data
```

Open `http://127.0.0.1:48931`; choose another port if occupied. Use a separate data directory for each test instance and never point tests at a user's saved library.

The host binds only to loopback, validates Host and Origin, requires an API session token and serves bundled assets only. Preserve these boundaries.

Without `--data-dir`, the host reads `gui-config.xml` and `gui-sources.xml` from its working directory and shares the normal ArchiveCatalog user-data store. Relative paths remain working-directory-relative. Do not run multiple frontends against the same saved data or download directory.

### Windows installer

Install Inno Setup 6 or 7, then package the validated executable, runtime DLLs, web assets, licenses and matching `payload` folder. With a prepared package:

```powershell
$version = (Get-Content -LiteralPath .\VERSION -Raw).Trim()
.\packaging\windows\build-installer.ps1 -PackageDir ".\build-windows-portable\ArchiveFetch-$version-windows-x64"
```

The installer helper does not assemble the portable package. It requires the packaged payload's version and hash to match the current PS5 build. Build the matching PS5 payload, then run the desktop `bundle_payload` target before assembling a distributable:

```powershell
cmake --build build --target bundle_payload
```

Keep the complete nested `extraction-licenses` tree and runtime dependency notices. Check source, unpacked packages, APK contents, binaries and notes for private information before distribution; scanning only a compressed installer is insufficient.

Installed shortcuts use `%LOCALAPPDATA%\ArchiveCatalog` as their working directory. When migrating from a portable build, close it before copying its configuration and source-list files; review relative paths. Installer upgrades retain the same per-user application identity and saved-data location.

## Android build

From the repository root:

```powershell
.\android\build.ps1
```

The helper looks under `$HOME\.archive-android` unless `JAVA_HOME` and `ANDROID_HOME` are configured. A fresh environment needs JDK 17, Android SDK 36, NDK `28.2.13676358`, CMake `3.22.1` and accepted SDK licenses.

With the toolchain configured:

```powershell
.\android\gradlew.bat -p android :app:assembleDebug :app:lintDebug
```

The helper copies the ARM64 development APK to `build/ArchiveFetch-arm64-debug.apk`. The application targets API 36 with minimum API 26. The Gradle wrapper and native downloads have checksum pins, and Maven dependency versions are locked. A clean cache needs build-time internet access.

Build the matching PS5 ELF before Android packaging if the APK should include console installation/update support. The payload is bundled only when its version matches [VERSION](VERSION).

### Android implementation boundaries

- Bundled HTML connects through an origin-restricted WebMessage/JNI bridge; do not enable arbitrary WebView navigation or file access.
- Native requests use curl/BoringSSL and an exported system trust bundle with peer and hostname verification enabled.
- Credentials use Android Keystore encryption and are excluded from cloud/device-transfer backup.
- Downloads stage in destination-isolated app-private directories and publish through MediaStore or a granted Storage Access Framework folder. Do not treat document URIs as filesystem paths.
- Replacement and deletion require ownership/content checks. Metadata-only library status must not weaken those destructive-operation checks.
- Provider support for rename and reported sizes matters. Test revoked grants and partial-publication failures without silently changing the selected destination.
- List each destination once per library refresh for unrecorded-file discovery; avoid a separate provider listing for every catalog entry.

## PS5 build

Use a Linux environment, such as WSL, with the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk) and its curl, libxml2 and OpenSSL ports. Configure the desktop CMake build first so pinned headers and web assets exist under `build/`.

Run in the Linux shell:

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make -C ps5
```

The result is `build-ps5/ArchiveFetch.elf`, with web assets, licenses, certificates and the extraction helper embedded. Platform code lives in `ps5/diag/` and shares the backend in `src/`.

### Platform constraints

- Link `SceSystemService` before `SceAppInstUtil`; loading AppInstUtil by itself can hang before entry.
- Firmware 13.60 lacks `sceAppInstUtilAppInstallTitleDir`. Tile registration uses `sceAppInstUtilAppInstallAll`, which also re-registers other applications staged under `/user/app/`.
- The default thread stack is 64 KiB. Preserve the pthread wrapper's larger stacks and per-thread signal handling.
- Rest mode can stop the HTTP accept loop; retain listener recovery.
- Extraction runs in a separate, short-lived ELF through the local loader. Do not replace that separation with heavy threaded work in a forked payload child.
- Preserve update handoff, authentication and Payload Manager ownership checks. Use Payload Manager's API rather than writing its storage directly.

PS5 data lives in `/data/archivefetch/`, with logs in `logs/run.log` and `logs/extract.log`. Never use a user's existing console data as a disposable fixture.

## Command-line client

Generate a catalog without downloading:

```powershell
.\build\archive_catalog.exe --url https://archive.org/details/YOUR_ITEM --output catalog-output
```

On Linux, use `./build/archive_catalog`. Add `--single-item` to skip related-item discovery. Alternatively:

```powershell
.\build\archive_catalog.exe --config config.example.xml
```

To select downloads explicitly, use exact item/name pairs from the catalog:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<archive_catalog output_dir="catalog-output" download_dir="downloads" follow_parts="true">
  <source url="https://archive.org/details/YOUR_ITEM"/>
  <download item="YOUR_ITEM" name="Your File.zip"/>
</archive_catalog>
```

XML-escape special characters. Paths are relative to the process working directory, not the configuration file. Every selected file must exist in the scanned catalog and have a checksum.

The CLI never implicitly downloads all files or overwrites conflicting existing files. Downloads use the filename directly under `download_dir`, not the source's remote directory structure. Files with the same basename therefore share a destination. Run only one instance against an output/download directory.

Catalog output includes XML, CSV and a URL list. XML has `complete="false"` and error details if discovery or a manifest fails. The CLI does not start downloads after an incomplete scan; CSV/URL exports may still be partial. Deduplication is by item and filename, not by hash or display name across sources.

For authentication, supply `IA_S3_ACCESS_KEY` and `IA_S3_SECRET_KEY` through a trusted process environment. Do not type literal secrets into commands or save them in XML. The CLI does not load the GUI credential store.

Exit codes: **0** success, **1** configuration/setup failure, **2** incomplete scan, **3** download failure. Interrupted transfers retain partial data for another run.

### Legacy headless PS5 payload

The root `make` target builds `archive_catalog.elf`, separate from the HTML PS5 app:

1. Put configuration at `/data/archive-catalog/config.xml`.
2. Supply a trusted CA bundle and set its absolute `ca_bundle` path. Never disable certificate checks.
3. Use absolute storage paths when appropriate and a filesystem supporting the required file size.
4. Load through a compatible loader, or use `make deploy PS5_HOST=YOUR_PS5_IP PS5_PORT=YOUR_LOADER_PORT` where supported.
5. Inspect `/data/archive-catalog/run.log` and exported catalogs. Relative paths resolve under `/data/archive-catalog`.

The launcher must supply environment credentials if needed. Stop the payload before editing its queue; relaunch to retry or resume.

## Testing

Run the checks relevant to a change, expanding coverage when shared behavior is affected:

```powershell
ctest --test-dir build --output-on-failure
```

The full Windows configuration registers ten suites, including catalog, transfer, application, GUI model, credentials, native extraction, Windows update, web API, Payload Manager and release-update tests. Disabled build options change the available set.

- The web smoke test uses isolated fixtures and loopback stand-ins for console control, pairing, merge and update flows.
- Payload Manager tests cover filename/slot preservation, unsafe destinations and upload failures without changing a real console.
- Update tests cover channel selection, constrained URLs, strict SHA-256 manifests, tampering, explicit installer handoff and persistence.
- `update_check_tests --live` is an optional GitHub check using an explicit older baseline. `--live-download` downloads and verifies a published ELF without executing it.
- `ps5/extract/host_test.sh` tests the PS5 extractor on Linux. Host tests do not replace real-console validation.

### Android instrumentation

**Use an isolated disposable emulator, not a phone with an existing library.** Tests replace application fixtures, and Gradle may uninstall the app.

```powershell
.\android\gradlew.bat -p android --project-cache-dir android\.gradle-arm64-v8a :app:connectedDebugAndroidTest
```

An emulator without ARM64 translation needs `-ParchiveAbi=x86_64` and a separate `--project-cache-dir android\.gradle-x86_64`. That variant uses `android/app/build-x86_64`, leaving ARM64 output separate.

Coverage includes native/API behavior, credential persistence, WebView rendering, settings, downloads, real folder selection/grants, replacement, revoked access, extraction and update flows. Live HTTPS is opt-in with `-Pandroid.testInstrumentationRunnerArguments.liveHttps=true`.

Real-console tests are separate and opt-in. `LiveConsoleUpdateTest#connectApprovedConsole` requires `allowLiveRead=true`, `liveConsole` and `livePassword`; it tests pairing, relay, release checking and reconnect without installing a payload. Supply credentials privately, never in committed commands or shared logs.

Use [tools/Test-IaS3Access.ps1](tools/Test-IaS3Access.ps1) only with an authorized account and target. A successful restricted-file test does not establish access to every restricted item.

APK signatures, native-library alignment, upgrade preservation, real system-installer handoff and physical-device behavior need checks beyond ordinary unit tests. Clearly distinguish emulator, stand-in and real-device results; do not claim one proves another.
