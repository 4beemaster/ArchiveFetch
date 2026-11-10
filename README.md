# ArchiveFetch

Current version: **0.7.3**, the same for the Windows, Android and PS5 apps. It lives in the `VERSION` file, which every build reads; the apps show it next to the ArchiveFetch name. Android's version code is derived from it (0.6.0-beta.1 = 6001, the 0.6.0 release = 6099) so each build installs over the earlier ones. Versions below 1.0 indicate an evolving, not-yet-stable project.

A Windows/Android HTML interface, a PS5 app served to the console browser and local network, desktop command-line client, and legacy headless PS5 ELF payload for combining Internet Archive file manifests and downloading explicitly selected files. The interfaces reuse the native catalog and download worker model.

Use downloads only for content you are authorized to download. The client supports Archive.org IA-S3 keys. Downloads are not automatically playable on PS5; emulation, conversion, and installation are outside this project.

## Behavior

- Accepts Archive.org item URLs or direct `_files.xml` URLs.
- Reads the source item's `_meta.xml` description to discover linked parts, including irregular identifiers. It does not guess A-Z URLs or crawl reviews.
- For identifiers containing `-part-`, follows only links with the same collection prefix. Other items follow description-linked Archive.org items. Discovery is one level deep and capped at 128 unique items; multiple explicit sources are supported.
- Fetches each canonical `_files.xml` and combines public and restricted entries of every file type, including files without an extension. **Hidden file types** (Settings → Library; `excluded_extensions` in the settings XML, default `.xml, .torrent, .sqlite, .sqlite3, .txt, .log, .jpg, .png`; commas or spaces both work) keeps matching files out of the Library unless they are downloaded. The eye-slash button next to a file type in the Extension column (shown on hover; always faintly on touch screens and the PS5) or in the Filters Extensions list hides that type at once, also during downloads, with an **Undo** in the notice. The catalog keeps hidden files, so removing a type shows its files again without a rescan. The command-line catalog export leaves hidden types out. Matching uses the last extension of the filename, ignoring case. Sources scanned before 0.5.0-beta.3 gain their metadata files on the next scan. Preserves source item, filename, byte size, MD5, SHA-1, and an encoded download URL. Private-marked entries remain listed with `restricted="true"` in XML and a `restricted` CSV column. This flag does not distinguish login-only access from other permission requirements.
- Deduplicates by item and filename, not by display name or hash across distinct items.
- Exports `catalog.xml`, `catalog.csv`, and `urls.txt`. The combined XML is an application catalog, not a byte-for-byte concatenation of the original manifests.
- Marks the XML `complete="false"` with error details if any discovery or manifest request fails. In the CLI, an incomplete scan exits with code 2 and does not start downloads. The GUI permits explicit downloads of available files from a partial catalog while preserving its source-error warning. CSV and URL exports may be partial; always check the XML status.
- Downloads only explicitly selected files, sequentially: checked catalog rows in the GUI or exact `<download>` entries in CLI configuration. There is no implicit download-all operation.
- Saves files directly in the selected download directory using only the filename, without collection or source subfolders. The remote URL and catalog retain the full original path.
- Resumes `.part` files using HTTP ranges, checks available space, then verifies size and the manifest's SHA-1 (or MD5 fallback) before renaming. Verified existing files are skipped; conflicting existing files are never overwritten. An integrity failure discards the partial file; an interrupted transfer keeps it for the next run. A server that does not honor range requests causes an error rather than appending a full response.
- Requires HTTPS certificate verification. Metadata requests have time, size, and redirect limits and up to two retries for transient connection errors or HTTP 408/429/500/502/503/504. Retries back off and honor `Retry-After` up to 60 seconds; longer requested delays are reported as failures for a later run. Certificate, authorization, parsing, and size-limit failures are not retried. Downloads have connection and low-speed timeouts but are not automatically retried; relaunch to resume. Transfers are sequential.

Archive.org's legacy MD5/SHA-1 values detect accidental corruption only. They are not modern authenticity checks and do not establish that an uploader or file is trustworthy. Static analyzers may flag these required compatibility fields; they are not used for credentials, signatures, or security decisions. Files without either checksum are listed but cannot be queued for download.

## Desktop Build

Requires CMake 3.20+, a C++17 compiler, libxml2, OpenSSL, and curl 7.85+. Windows with MinGW is supported. CMake can build the pinned, SHA-256-checked curl 8.18.0 release if curl development files are unavailable. Bundled curl uses OpenSSL, matching the PS5 ports; on Windows, it validates certificates against the Windows trust store. This avoids dependence on Schannel's online revocation service; no certificate-verification bypass is used.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUNDLED_CURL=ON
cmake --build build
ctest --test-dir build --output-on-failure
.\build\archive_catalog.exe --url https://archive.org/details/ps2-redump-usa-chd-part-0 --output catalog-output
```

The command above generates metadata lists only. Add `--single-item` to skip related-part discovery. On Linux, use `./build/archive_catalog` instead. For offline parser-only builds use `-DBUILD_APP=OFF`; libxml2 is still required. Use `-DBUILD_WEB=OFF` to build only the CLI and core tests. HTML builds fetch SHA-256-pinned cpp-httplib 0.18.3, nlohmann/json 3.11.3, Karla, Font Awesome Free and qrcode-generator assets and, on Windows, webview 0.12.0 and WebView2 SDK 1.0.1150.38. The SDL2/Dear ImGui desktop app was retired in 0.6.0; the HTML interface replaces it. Downloads happen at build time, never at application startup. If your build machine uses a private certificate authority, configure CMake's trusted CA bundle (`CMAKE_TLS_CAINFO`) rather than disabling TLS verification.

## Windows HTML Interface

From the project directory:

```powershell
.\build\archive_catalog_web.exe
```

This opens an integrated WebView2 window, not the default browser. The Microsoft Edge WebView2 Runtime must be installed. Keep the generated `web` directory beside the executable, including its fonts and license notices. For a source build, native runtime dependencies (compiler runtime, libxml2, OpenSSL) must be available on `PATH`. The release's Windows installer bundles those dependencies.

- Sources supports adding Archive.org links, optional related parts, persistent custom tags, individual selection/removal, scanning, and clearing the source list without deleting the library. Source selection applies only to visible rows.
- Sources and Library include a Filters menu with type-ahead multi-select dropdowns, removable active-filter chips, and sorting with A-Z/Z-A direction choices. Clicking a column header also sorts by it; clicking the sorted header again reverses the direction. Library separates inherited Source Tags from editable file-specific Tags; its Tags filter matches either column. File tags are saved locally by Archive.org item and full filename, independently of catalog imports and exports. Library also includes filename search, an Extension column (the last `.xxx` of the filename, so `.n64.zip` shows `.zip`), source/extension/access/favorite/download-status filters, favorites, XML import, catalog export, and paged file selection. Existing-file replacements require confirmation. Queue additions use catalog revisions to reject stale selections.
- Collections group sources on the Sources page. **Add to collection** puts the selected sources into an existing or new collection; a source belongs to at most one collection, so adding it elsewhere moves it. Each collection has a name and tags, edited with the button next to its name, and is shown as a collapsible heading with a checkbox that selects all its sources. Sources in a collection share the collection's tags and cannot be tagged on their own. When sources with their own tags join, you choose to add those tags to the collection, discard them, or pick some; when sources leave or a collection is deleted, you choose to remove the collection's tags from them, keep them, or pick some. A warning appears when a choice removes a tag that a download location uses. In Library, the Source column, sort and filter use the collection name, with the Archive.org item in the tooltip. Collections are saved in `collections.json`; member tags are also written to the source list, so older versions keep the same tags.
- Downloads uses the existing native transfer worker, not browser downloads. Start/retry, Stop, live removal, checksums, resumable partials, and safe replacement retain the model's existing behavior. Current item/percentage appears beneath Downloads in the sidebar on every page. Normally completed entries clear automatically; failures remain for retry.
- Sources can be added, tagged, grouped and scanned while downloads run; scans leave the queue alone. Files can also be queued while a download runs: the running queue picks them up in order, and **Retry** on a failed file puts it back into the running queue. File tags, favorites, Clean library and settings still wait for downloads to finish. While a scan runs, the header button reads **Stop scan** and stops only the scan; press Stop again to stop downloads.
- A scan skips files whose names cannot be saved safely (such as `..`, `:` or `?`) and lists them in the scan message instead of failing the whole source. Archive.org's `history/files/` copies of replaced files and empty files are left out silently.
- Settings includes a Download locations list with nicknames, paths, tags, native folder pickers, and a default-location radio choice. The built-in Default location can change its path but cannot be renamed or removed. Location tags have one owner, ignoring ASCII case. Location preference offers Auto (same as File Tag), File Tag, Source Tag, and Default. Auto/File Tag try file tags, then source tags, then the configured default; Source Tag reverses the tag order; Default bypasses tags. The Library Location dropdown remembers its last choice, supports adding locations, and explicitly selected locations always override the preference. Conflicting matches at the winning tag priority require a choice before any files are queued.
- Desktop download history records each successful file's actual path independently of the current default or location list. Editing/removing locations does not delete files, discard history, or retarget queued downloads/retries. Library status checks all recorded copies and displays a shortened path with full paths in its tooltip. Missing, unavailable, and changed copies are distinguished; any valid copy counts as downloaded. The trash button deletes a downloaded copy; on a missing or unavailable copy it removes only the record. **Clean up missing downloads** under Download locations appears when a recorded copy is missing or unavailable. It removes the records of copies deleted from a folder that still exists; records in unavailable locations, such as an unplugged drive, are only removed when you tick the option in its confirmation. Transfers verify checksums; subsequent status refreshes compare file size and modification time, not a fresh checksum. Existing downloads in the current default folder are adopted only when their basename is unambiguous in the catalog. History cannot identify unrecorded files in previously used folders.
- **Clean library** (Library toolbar) applies to the selected files or the whole Library (hidden file types included) and has two choices, each showing its count first. **Delete downloads from storage** deletes copies verified as downloaded, with their extracted folders, and shows the space it frees; the entries stay and can be downloaded again. **Remove from the Library** removes **Only never downloaded** files (never downloaded or whose copy is missing; partly downloaded ones stay), **Only without a source** (source deleted and not downloaded) or **All of them** (entries only; files on disk show as downloaded again after a scan). Ticking both does both, and "never downloaded" keeps the entries whose downloads were just deleted. Deleting asks a second time and lists what will be deleted; it is not available while downloading. A file counts as downloaded while any copy is downloaded, extracted, changed or on an unavailable location (such as an unplugged drive); unavailable and changed copies and queued files are never touched. Sources, collections and settings are never touched. The Library Source filter has a **Source removed** choice, and the trash button on such a file offers to remove its entry too.
- Settings also includes automatic start, masked S3 key entry, and optional Windows DPAPI persistence. The CA bundle is not exposed in Settings; existing configuration-file CA settings are preserved. Saved keys are never returned to JavaScript. Diagnostics includes source/queue errors and a bounded 64 KiB network log with copy/clear controls. Android retains its existing system download-folder picker; named desktop locations are not enabled there.

The HTML host reads the existing `gui-config.xml` and `gui-sources.xml` from the working directory and shares the normal library, favorites, and credential store under the user's ArchiveCatalog data directory. Relative configured paths remain working-directory-relative. Run only one frontend against the same saved data and download directory at a time.

The server binds only `127.0.0.1`, using an available random port by default. It checks Host and Origin, rejects cross-site requests, requires a random session token for API calls, and serves only bundled web assets. The integrated window blocks navigation to other origins and new windows. There is no external hosting or LAN access; another device cannot use this server to download files onto itself.

For browser-based development, run the host without its native window:

```powershell
Start-Process .\build\archive_catalog_web.exe -ArgumentList '--no-browser --port 48931' -Wait
```

Then open `http://127.0.0.1:48931`. Use a different port if occupied. `--data-dir PATH` isolates settings, sources, library, favorites, and credentials for testing. The offline `--smoke-test` option creates synthetic files in an isolated directory and tests real HTTP routes, session/origin/host guards, secret exclusion, overwrite confirmation, queue commands, and restart persistence. It is also registered as `web_api_tests` in CTest. An explicitly supplied smoke-test data directory must be empty.

### Windows installer

The Windows setup EXE installs for the current user without administrator access, with Start menu and optional desktop shortcuts. Run a newer setup EXE to upgrade the same installation. Settings, pairing, credentials, library records and downloaded files are not part of the installed-file set and are not deleted by uninstall. The installer does not check GitHub for updates; download and run each newer installer manually.

Installed shortcuts use `%LOCALAPPDATA%\ArchiveCatalog` as their working directory, keeping `gui-config.xml` and `gui-sources.xml` outside the install folder alongside the existing application data. When moving from a portable/development build for the first time, close the old app and copy those two files from its previous working directory if needed; existing relative download/export paths may need updating. Later installer upgrades keep that location unchanged. Do not run multiple frontends against the same saved data.

Microsoft Edge WebView2 Runtime must be installed. Setup checks for it and, if missing, offers the official download page rather than silently fetching or installing another program. Install the Evergreen Runtime and rerun setup. Beta installers are unsigned; Windows may display an unknown-publisher warning.

To build setup from a validated portable package (EXE, runtime DLLs, web assets, licenses and matching `payload` folder), install Inno Setup 6 or 7 and run:

```powershell
.\packaging\windows\build-installer.ps1 -PackageDir .\build-windows-portable\ArchiveFetch-0.7.3-windows-x64
```

The script requires the bundled payload version and hash to match the current PS5 build. Privacy-scan all staged files and the resulting installer before distributing either. The stable setup application ID keeps beta/release upgrades in the same per-user installation.

Android uses the separate native wrapper below, not the Windows HTTP host.

## Android APK

Library is the default page. Switching apps and rotating the device retain the current WebView, including its selected page and in-memory controls. If Android recreates the activity, saved WebView navigation restores the selected page; this does not persist the download queue across process termination.

Version 0.1.0 (code 10) adopts the ArchiveFetch name and development version numbering. Settings includes **Get S3 keys**, opening the Internet Archive login/key page in the system browser. Settings identifies loaded saved or session keys without exposing their values; blank key fields retain loaded keys. ArchiveFetch supports all file types; Hidden file types decides which ones the Library shows. Rescan existing sources to add files omitted by older CHD-only scans. Anonymous attempts for restricted catalog entries remain permitted when both S3 key fields are blank and no keys are loaded. Supplied keys are still used; incomplete key pairs remain invalid. Archive.org decides whether access is permitted, and HTTP 401/403 is reported as a normal failed download, not bypassed.

The ARM64 development APK is `build/ArchiveFetch-arm64-debug.apk`, suitable for sideloading on a Pixel 10 Pro XL. It supports Android 8/API 26 and newer and targets Android 16/API 36. It is debug-signed, not a Play Store release. Transfer the APK to the phone, open it in Files, allow installation from that source when Android asks, and install ArchiveFetch. No PC or external web server is needed to run it. An up-to-date Android System WebView is required. The package ID and private saved-data locations remain unchanged, so this installs over Archive Catalog, PS5 ArchiveBridge, or ArchiveBridge without losing settings or credentials. Desktop executable filenames remain unchanged for compatibility.

- HTML, fonts, icons, and license notices are bundled in the APK. AndroidX's HTTPS asset loader and origin-restricted WebMessage listener connect the UI to the native C++ backend through JNI. WebView file/content access, external navigation, cleartext traffic, and network resource loading are blocked. Archive.org requests use native curl/BoringSSL with certificate verification against a bundle exported from Android's installed trust store; no verification bypass is used.
- Version 1.4 removes the unnecessary **All files access** requirement and legacy read/write storage permissions. App-specific downloads require no storage permission; selecting a shared destination grants access only to that folder through Android's picker. Other apps' private files remain protected.
- On Android 10 and newer, the initial destination is the phone's public **Downloads/ArchiveFetch** folder, created on the first successful publication using MediaStore without broad storage permission. Only this default adds the app-named subfolder. In Settings, use the folder button beside Download directory, choose a folder in Android's picker, approve **Use this folder / Allow**, then Save settings. A custom folder is used exactly as selected, with no extra subfolder. Android remembers that folder grant, and updates preserve an existing custom selection. Android may prohibit choosing the Downloads root itself: select a subfolder instead. Finish or remove queued downloads before changing folders. Legacy default-path settings use the new default, but existing files are not moved automatically; older downloads remain in **Downloads/PS5 ArchiveBridge** or **Downloads/ArchiveBridge**, which can be selected explicitly through the folder picker. Android 8-9 retains app-specific storage until a custom folder is selected.
- Diagnostics includes the installed app version, private/staging paths, selected-folder publication stages, and actionable errors. Temporary TLS backend, certificate-parser, and per-request timing events have been removed. An unreadable or invalid CA bundle produces a concise warning; HTTPS certificate and hostname verification remain enabled. The native log remains capped at 64 KiB and supports Copy/Clear. Keys, authorization headers and response bodies are not logged. Paths can reveal local filenames, so review diagnostics before sharing publicly.
- Native downloads and resumable partials stage in the app-specific `Android/data/org.archivecatalog.app/files/Download` directory, with internal storage as a fallback. After checksum verification, completed files are copied to the public default or selected SAF folder and the staged final file is removed. **Allow room for a staged file plus its destination copy.** Failed or cancelled copies retain verified staging data for retry and clean up their destination temporary file. Existing destination files require overwrite confirmation and are backed up during replacement; custom providers must support rename and report file sizes. MediaStore listing/replacement is limited to this installation's owned files in the exact default folder; files left by a previous uninstall may require selecting that folder through the picker. The local Android document provider is tested; arbitrary cloud providers are not. Library status and Share use the destination folder. Public downloads remain after uninstall; app-specific downloads and staging files are deleted on uninstall/clear-data. Export shares XML/CSV/URL outputs. Import uses Android's document picker and copies the selected XML into private storage; content URIs are never treated as filesystem paths.
- Android export/trust paths remain managed by the app. S3 keys are session-only by default; Remember keys encrypts them with AES-256-GCM using a non-exportable Android Keystore key. Credentials are never returned to HTML or saved to XML. Cloud/device-transfer backup is excluded. Forget removes the encrypted saved copy and current session values. This does not protect against a compromised phone or privileged process inspection.
- User-started operations run in a foreground data-sync service with a progress notification and Stop action. Android's system limits still apply, including its data-sync time budget, force-stop, and process termination. Partials survive cancellation, but queues are not persisted; after restarting, reselect interrupted files to resume. Multi-hour background behavior and real authenticated transfers still need testing on the physical phone.

Build on this machine:

```powershell
.\android\build.ps1
```

The helper finds the per-user toolchain under `$HOME/.archive-android` unless `JAVA_HOME`/`ANDROID_HOME` are already set. A fresh machine needs JDK 17, Android SDK 36, NDK `28.2.13676358`, CMake `3.22.1`, and accepted Android SDK licenses. With those environment variables configured, `android/gradlew.bat -p android :app:assembleDebug :app:lintDebug` builds directly. The Gradle wrapper is pinned to 8.11.1 with its official SHA-256; Maven dependency versions are locked. Native source archives and font assets have SHA-256 pins. TLS libraries are compiled before curl's configure-time link checks. Build-time internet access is required on a clean cache.

Runtime instrumentation passed on an isolated Android 16/API 36 emulator, both x86_64 and the ARM64 APK through the emulator's ARM64 translation. It covers session rejection, encrypted credential save/reload/forget, secret exclusion, Unicode filenames, XML import, stale selections, native existing-file checksum verification, queue clearing, overwrite confirmation, favorites/settings restart persistence, export paths, and actual WebView rendering/filtering. APK signatures and 16 KiB native-library alignment are checked separately. No physical Pixel compatibility or authenticated download success is claimed yet.

The ARM64 folder workflow also exercises Android's actual folder picker and persisted URI grant, verified-file publication, selected-folder status/sharing, checksum-failure preservation, approved replacement, and revoked-access errors. Live HTTPS testing is opt-in with `-Pandroid.testInstrumentationRunnerArguments.liveHttps=true`; normal tests do not require Archive.org access.

Version 1.3 fixes the trust-anchor failure caused by the old PEM exporter inserting a blank line before a certificate's end marker when its Base64 length was divisible by 64. BoringSSL rejects that form with PEM `NO_DATA` and curl code 77. Export now uses Java's Base64 encoder with 64-character lines and no extra blank line. The regression test reproduces rejection of the broken form, accepts the corrected bundle, and the strict live HTTPS test passes on Android 16. The app regenerates the bundle when its service starts; install the update and reopen the app without clearing its data. HTTPS peer and hostname verification remain enabled. Physical Pixel confirmation is still pending.

To repeat instrumentation, use an **isolated disposable emulator**, not a phone with an existing library: tests replace its app library with fixtures and Gradle may uninstall the test app. Run `android/gradlew.bat -p android --project-cache-dir android/.gradle-arm64-v8a :app:connectedDebugAndroidTest`. For an emulator without ARM64 translation, use `-ParchiveAbi=x86_64 --project-cache-dir android/.gradle-x86_64`; separate task-history caches avoid Android Gradle plugin incremental DEX errors when switching ABI output directories. That variant writes `android/app/build-x86_64` and leaves the default ARM64 APK intact.

## Configuration and Queue

The supplied `config.example.xml` scans the example collection without downloading games. Run it directly on desktop:

```powershell
.\build\archive_catalog.exe --config config.example.xml
```

For selected downloads, add entries using exact `item` and `name` attributes from `catalog.xml`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<archive_catalog output_dir="catalog-output" download_dir="downloads" follow_parts="true">
  <source url="https://archive.org/details/YOUR_ITEM"/>
  <download item="YOUR_ITEM" name="Your File.chd"/>
</archive_catalog>
```

XML-escape special characters, such as `&amp;` in filenames. Repeated selections are deduplicated. Every selection must be present in the scanned catalog and have a checksum before any queue transfer begins. Download paths are `download_dir/filename`, even when the remote filename contains folders. Directory traversal, symlink destinations, and Windows-unsafe filenames are rejected. Without explicit GUI overwrite approval, an existing matching file is verified and skipped; a mismatching file produces an error rather than being overwritten. CLI downloads retain this non-overwriting behavior. If multiple selections share a filename, they share a destination; explicitly approved replacements run sequentially and the last successful replacement wins. Only run one instance against a given output/download directory.

Existing downloads and `.part` files in older collection subfolders are not moved automatically. To reuse them, move the desired file or partial file directly into the selected download directory while the app is stopped, checking for filename conflicts first.

Relative output, download, and certificate paths are relative to the process working directory, not the configuration file's directory. A third-party uploader's description is not proof of collection completeness: a successful scan means all discovered manifests were processed, not that every released game is included. Files can become unavailable between scanning and downloading.

## Archive.org Login

Some items expose metadata publicly but require an account to download the files. A complete catalog is not proof of permission to download every entry. The plain URL list does not carry access flags; consult XML or CSV before queuing files.

Get your account's IA-S3 access and secret keys at <https://archive.org/account/s3.php>. In the GUI, enter both under Settings > Archive.org IA-S3. Keys are session-only unless you opt into Remember keys on Windows and save settings. Never paste keys into chat, commit them, or put them in command arguments or XML files. S3 keys can authorize account writes as well as downloads, so treat both as sensitive credentials.

Windows stores only a DPAPI-encrypted blob in `%LOCALAPPDATA%\ArchiveCatalog\credentials.bin`, with no separate application encryption key or password prompt. The normal Windows user-profile permissions apply. This protects a copied credential file from ordinary use under another account or machine; it does not protect against malware running as your user, administrators controlling your session, or memory inspection while the keys are in use. Saved credentials are not portable to PS5. If the file cannot be unlocked, the app reports an error and remains usable for manual key entry or Forget keys. Temporary serialization and decrypted DPAPI buffers are explicitly wiped, but the app does not claim that every runtime copy of a key is erased. CLI authentication is unchanged and does not load the GUI credential store.

For CLI use, supply `IA_S3_ACCESS_KEY` and `IA_S3_SECRET_KEY` in the process environment through a trusted local mechanism. Do not type literal secrets into commands saved in shell history. Programmatic callers can supply `Config::s3`. The legacy headless PS5 build requires its launcher to supply the environment. The Windows and Android interfaces provide masked key entry.

Authenticated requests send `Authorization: LOW access:secret` over verified HTTPS. A pre-request destination check restricts these requests, including redirects, to `archive.org` and its subdomains on port 443. Keys are never included in catalog exports or settings XML. Both keys are validated before use, and restricted selections without them fail before the queue starts. HTTP 401/403 is reported rather than bypassed or retried.

An account-holder test using `tools/Test-IaS3Access.ps1` confirmed a restricted CHD returned HTTP 401 anonymously and HTTP 206 with a valid CHD header using S3 keys alone. This demonstrates access to that file, not all restricted items or borrowing-protected content. The probe requests at most 4 KiB per test and prompts for keys without displaying or saving them.

IA-S3 is the only supported authentication method. Browser-session authentication and its configuration have been removed. Older configuration files still load, but obsolete attributes are ignored and are not written when GUI settings are saved again.

## Controlling a PS5 from Windows or Android

The switcher at the top of the sidebar in the Windows and Android apps chooses **This device** or a saved console. Add a console with a name, its address (an IP address or host name, as shown in the console's **Connect from another device**) and port (8890). Each console shows a status dot: online, needs sign-in, offline, refused, or running an older ArchiveFetch. **Find consoles on this network** checks every address of this device's own home network (a private 10.x, 172.16-31.x or 192.168.x /24) on port 8890 only, asking `/session` without any key, cookie or password, and lists the consoles running ArchiveFetch with an **Add** button; it never touches the loader port and does not run on other kinds of networks. The Windows and Android apps carry the PS5 payload of their own version (`payload/` next to the Windows app, inside the APK; bundled only when `build-ps5` holds an ELF built from the same `VERSION`). A console running an older ArchiveFetch gets an **Update ArchiveFetch** button, and one that does not answer an **Install ArchiveFetch** button: after you confirm, the app sends the payload to the console's ELF loader (port 9021 by default, **Loader port** per console) and waits until `/session` reports the new version; the running ArchiveFetch hands over to the new one. Start the console's loader first. A console running ArchiveFetch 0.7.2 or later takes the update itself: the app uploads the payload to it on port 8890 (`POST /api/update`, with the same sign-in, pairing and session rules as every other request), and ArchiveFetch hands it to the loader on the console's own 127.0.0.1, so the loader port never has to be reachable from other devices or the internet. Only the first install, or a console that does not answer, needs the loader port. A banner at the top of the page shows when the console being controlled (or, on This device, a saved console) runs a different ArchiveFetch: an older one with **Update ArchiveFetch** and its progress, a newer one with a note to update this app instead, since an app only carries its own version. Dismissing the banner hides it until either version changes.

**Sources > Merge** (Windows and Android apps, once a console is saved) copies from this device to a console or from a console to this device: the selected sources (on the side you merge from) or all of them, with any of sources and collections, the Library catalog, file tags and favorites. **Preview** lists what would be added and every tag difference (a source or file tagged differently on both sides), each with **Add tags** or **Keep as is**, plus Add all and Keep all. Merging only adds: new sources (unscanned, joining a collection of the same name or a new one), catalog files of sources the other side has, missing tags and favorites; nothing is removed, renamed or replaced, and settings, keys, download locations, the queue and download status are never copied. Sources are matched by link, files by source and name, collections by name. Large merges are sent in parts; each part only adds, so after an interruption a new preview continues where it stopped. With a console chosen, Sources, Library, Downloads and Settings show and change the console; Export writes on the console, Import and Share are not offered, and display preferences, the console list and Diagnostics stay on this device. The app's backend sends each request to the saved address over plain HTTP; the page never talks to the console itself. A console outside this device's network asks for its password the first time. The first time the app controls a console (0.7.0 or later), it **pairs**: the console gives it a permanent device key, so later connections, including after restarting the app and from outside the console's network, need no password. The key is saved encrypted (Windows DPAPI, Android Keystore); the console keeps only a SHA-256 fingerprint in `/data/archivefetch/paired-devices.json` and lists paired apps in **Settings > Network > Paired devices**, where **Remove** revokes one. A removed app does not pair again on its own; it pairs again after signing in with the password, or after the console is removed from and added back to the app's list. Changing the password does not remove paired apps. The list is saved in `consoles.json` in the app's data folder.

Starting with 0.7.3, an app update also replaces the console's existing ArchiveFetch copy in itsPLK's Payload Manager, so a reboot does not bring back the old payload. After the new ArchiveFetch starts, the app sends its payload once more to `POST /api/payload-manager/update`; this works even when upgrading from 0.7.2, whose updater does not know about Payload Manager. The console uses Payload Manager's own loopback HTTP API on port 8084, preserving the existing filename, slot and boot list. No copy is added when none exists; USB copies, repository-managed entries, ambiguous duplicates and folders shared with other payloads are not overwritten. If synchronization fails, the running update stays in place and the app keeps a warning visible, with **Retry update**. Sending an ELF directly to port 9021 without the app does not synchronize Payload Manager.

## PS5 App

Version **0.7.3**. ArchiveFetch runs on a jailbroken PS5 as a payload ELF and serves its web interface on port **8890**, to the console's own browser and to phones or computers on the same network. Verified on firmware 13.60.

### Install and open

1. Send `ArchiveFetch.elf` to your ELF loader (commonly port `9021`). A notification says when it is ready.
2. Open **ArchiveFetch** in Media; the first launch registers that tile. On another device, open `http://CONSOLE-IP:8890/`, or press the QR button at the top of the page and scan the code.
3. The payload stops when the console restarts; send the ELF again after each reboot. It keeps running through rest mode.

Sending a newer (or the same) ELF while one is running replaces it: the new copy asks the old one to stop, waits for the port, and takes over. Keys that were entered but not saved carry over to the new copy in memory.

### What it does on the console

- The same Sources, Library and Downloads screens as the desktop app, including tags, filters, favorites, Tag selected, source collections and named download locations.
- Downloads of files of 32 MiB or more are split into several connections at once (**Settings > Storage > Connections per download**, 1 to 8, 4 by default, on every platform), resume per part after stopping, and are joined and checksum-verified before the file is kept. A part that archive.org refuses or drops waits and continues on its own (up to 5 times, honouring `Retry-After`) while the others keep going; archive.org starts refusing parallel requests from one client at around 10 to 12. Downloads shows the speed and time left while a file transfers. The queue and the sidebar show each step after the transfer (Joining parts, Verifying, Extracting) with its own yellow progress bar and percentage.
- The folder chooser lists internal storage (`/data`), extended storage (`/mnt/extN`) and USB drives (`/mnt/usbN`) that are actually mounted, with free space.
- Settings > Display > Text size defaults to 150% in the PS5 browser. The menu can collapse to icons.
- HTTPS certificates are checked against Mozilla's CA list (curl.se, pinned by date and SHA-256), built into the ELF and written to `/data/archivefetch/cacert.pem`.

### Extracting archives

Downloaded `.zip`, `.7z`, `.rar` and `.tar` archives (also `.tar.gz`, `.tgz`, `.tar.bz2`, `.tar.xz`, `.tar.zst`) can be unpacked automatically into a folder with the archive's name, next to it. **Settings > Always extract archives after download** does it for every location; a location's **Extract archives downloaded here** does it for that location only. **Delete the archive after extracting** (off by default) removes the archive once its folder is complete; the Library then shows the file as **Extracted**.

- The unpacking runs in a separate short-lived program, `ArchFtchX.elf`, that ArchiveFetch carries and hands to the console's own ELF loader on port 9021 for each archive, so a damaged archive cannot take ArchiveFetch down. The loader must be running on that port.
- Names that would leave the folder (`..`, absolute paths, drive letters) are skipped, as are links and special files; characters an exFAT drive cannot hold become `_`. A message says how many were skipped.
- If the folder already exists, nothing is extracted. A failure, a full drive or **Stop** removes the folder that run created; the archive is always kept then.
- Not supported: password-protected archives and multi-part sets (`.part1.rar`, `.r00`, `.7z.001`); those downloads are kept as they are.
- The extractor logs to `/data/archivefetch/logs/extract.log`; ArchiveFetch logs `EXTRACT_BEGIN`/`EXTRACT_END` in `run.log`. The trash button on an extracted file deletes the archive and its extracted folder (or the folder alone when the archive was deleted after extracting). An extracted folder counts as downloaded in any download location, also without a download record (for example after the file was removed from the Library and scanned again), and downloading the file again asks first, because extraction never overwrites an existing folder.

### Network access

By default only the console itself and devices on its local network (`127.x`, `10.x`, `172.16-31.x`, `192.168.x`, `169.254.x`) can use ArchiveFetch; anything else is refused, including the page itself. **Settings > Network > Allow external connections** turns that off. It needs a password first, can only be changed from the local network, and is saved in `/data/archivefetch/network.json`.

The password (8-128 characters) is set, changed or removed in **Settings > Network** from the console or its local network; only a salted PBKDF2-SHA256 hash is stored. Local devices never need it. An outside device gets only the page and a sign-in screen until it signs in; sign-in lasts 30 days on that browser, including across console restarts and ArchiveFetch updates (only a SHA-256 fingerprint of each sign-in is saved, in `/data/archivefetch/sign-ins.json`), **Sign out of this device** ends it, and changing the password signs every outside device out. Five wrong passwords lock that address out for 15 minutes, and twenty from anywhere within an hour pause outside sign-in for an hour with a console notification. The page is still plain HTTP: on untrusted networks the password, the sign-in cookie and keys typed into the page can be read in transit, so prefer a VPN into your network when you have one.

### Archive.org keys

Keys are needed only for restricted files. **Get S3 keys** opens the Archive.org keys page in a new tab, or in the PS5 browser shows the QR code so you can continue on a phone. **Remember keys on this device** saves them in `/data/archivefetch/credentials.bin`, readable only by its owner but **not encrypted**: the console has no secret store a payload can use. Anything that can read the console's files, such as an FTP payload, can read them. Forget saved keys deletes the file. Keys can be replaced at any time on the Archive.org keys page.

### Logs

The log is `/data/archivefetch/logs/run.log`, one JSON object per line. At startup a log over 1 MiB is moved to `run.previous.log`. **Settings > Console log** shows the sizes, opens a searchable table of the newest 1000 entries (problems in red), and can clear the log. To report a problem, send `run.log`; it contains local addresses, paths and filenames but never keys.

Saved data (sources, library, settings, tags, locations, download history) lives in `/data/archivefetch/`. Builds before 0.3.0 logged to `/data/archivefetch-diag/run.log`; that folder can be deleted.

### Build

Requires a Linux build environment (WSL works), the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk) with its curl, libxml2 and OpenSSL ports, and a desktop CMake configure first so the pinned cpp-httplib/JSON headers and web assets exist under `build/`.

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make -C ps5
```

This produces `build-ps5/ArchiveFetch.elf` with the web interface, fonts, icon, license notices and CA list built in. The source is in `ps5/diag/` plus the shared backend in `src/`.

Console-specific details the code depends on:

- `-lSceSystemService` must be linked before `-lSceAppInstUtil`; AppInstUtil alone hangs before `main`. AppInstUtil is never loaded with `dlopen`.
- Firmware 13.60 lacks `sceAppInstUtilAppInstallTitleDir`, so the tile is registered with `sceAppInstUtilAppInstallAll`, which also re-registers other apps staged under `/user/app/`.
- Threads default to 64 KiB stacks on the console; `pthread_create` is wrapped at link time to give every thread 512 KiB and its own signal stack, so a crash is logged as `FATAL_SIGNAL` instead of silently ending the process.
- Rest mode stops the HTTP accept loop; the listener rebinds every 5 seconds until it can.
- The console grants at most a 512 KiB TCP receive buffer, which limits one connection to roughly 6 MB/s; hence the parallel connections.

## Legacy Headless PS5 Payload

The root `make` target remains separate and produces `archive_catalog.elf`, a command-line style payload with no web interface; it is not the PS5 app above. Its configuration and launch instructions are:

1. Place your configuration at `/data/archive-catalog/config.xml` using your existing file-transfer tools.
2. Place a trusted CA certificate bundle at `/data/archive-catalog/ca-bundle.crt` and set `ca_bundle="/data/archive-catalog/ca-bundle.crt"` on the configuration root. The PacBrew curl installation includes a CA bundle under its target homebrew `etc` directory; locate it in your SDK bundle. Do not disable certificate checks.
3. Use absolute output/download paths if storing on USB; verify that your loader can access that mount. Use a filesystem that supports files larger than 4 GiB, such as exFAT, when required.
4. Load the ELF using your existing compatible loader. Where it accepts SDK network deployments, use `make deploy PS5_HOST=YOUR_PS5_IP PS5_PORT=YOUR_LOADER_PORT`. Port 9021 is only a default; use your loader's actual port.
5. Inspect `/data/archive-catalog/run.log` and the exported catalogs. Relative paths on PS5 resolve under `/data/archive-catalog`. Relaunch to retry failures or resume interrupted downloads; stop the payload through your loader before editing the queue.

Exit codes: 0 success, 1 configuration/setup error, 2 incomplete scan, 3 one or more failed downloads. Scan and download progress is written to standard output on desktop and `run.log` on PS5. Log files append across launches.

## Testing

- **Windows** (`ctest --test-dir build`, seven suites): catalog parsing and import/export, hidden file types, path safety, checksums, configuration, the queue and partial downloads, S3 key handling, the DPAPI credential store (using temporary files, never real secrets), and the web API smoke test (`archive_catalog_web --smoke-test`), which also covers console control, pairing, the network scan, Install/Update, boot-copy warnings/retries and Merge against stand-in consoles on this computer. `payload_manager_tests` uses a loopback stand-in to test Payload Manager 0.5/0.6 replacement, filename/slot preservation, refused unsafe destinations and upload failures without touching a console or boot list.
- **Android** (instrumentation tests on an emulator): the native backend, settings and credential storage, downloads and the folder picker, the WebView interface, console control and pairing, the bundled PS5 build, and live HTTPS to archive.org.
- **PS5**: `ps5/extract/host_test.sh` runs the archive extractor on Linux; each release is also tried on a real console.
- Restricted downloads with Archive.org keys were confirmed with `tools/Test-IaS3Access.ps1`, not for every kind of restricted item.

## License

Copyright (C) 2026 4beemaster. ArchiveFetch is free software under the GNU General Public License v3.0 or later; see [LICENSE](LICENSE). Source code: https://github.com/4beemaster/ArchiveFetch. Third-party components and their licenses are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

ArchiveFetch is an independent project, not affiliated with or endorsed by Sony Interactive Entertainment or the Internet Archive. "PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment Inc. Use it only on consoles you own, and download only content you have the right to download. It comes with no warranty; see the license.
