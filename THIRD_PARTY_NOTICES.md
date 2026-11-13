# Third-Party Notices

ArchiveFetch is licensed under the GNU General Public License v3.0 or later (see [LICENSE](LICENSE)). Its builds include the components below, each under its own license. None of their source code is stored in this repository: the builds download fixed versions, checked by SHA-256 where the build files list one, or take them from the PS5 SDK. Each license applies to its component only.

| Component | Version | Used in | License |
|---|---|---|---|
| [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) (C library, startup code, system stubs) | release installed 2026-10 | PS5 | GPL-3.0-or-later; `include/freebsd` BSD |
| [LLVM libc++ / libc++abi](https://libcxx.llvm.org/) (installed by the PS5 SDK) | as built by the SDK | PS5 | Apache-2.0 WITH LLVM-exception |
| [curl / libcurl](https://curl.se/) | 8.18.0 | PS5, Windows, Android | [curl license](https://curl.se/docs/copyright.html) (MIT-style) |
| [libxml2](https://gitlab.gnome.org/GNOME/libxml2) | 2.13.5 (PS5), 2.15.3 (Android), system (Windows) | PS5, Windows, Android | MIT |
| [OpenSSL](https://www.openssl.org/) | 3.5.2 (PS5), system (Windows) | PS5, Windows | Apache-2.0 |
| [BoringSSL](https://boringssl.googlesource.com/boringssl/) | 0.20261020.0 | Android | Apache-2.0 (some files ISC / OpenSSL license) |
| [libarchive](https://www.libarchive.org/) | 3.7.4 (PS5), 3.8.2 (native) | PS5, Windows, Android extractors | BSD-2-Clause |
| [xz / liblzma](https://tukaani.org/xz/) | 5.4.6 (PS5), 5.8.1 (native) | PS5, Windows, Android extractors | Public domain (PS5); 0BSD and component licenses in COPYING (native) |
| [Zstandard](https://facebook.github.io/zstd/) | 1.5.6 (PS5), 1.5.7 (native) | PS5, Windows, Android | BSD-3-Clause |
| [zlib](https://zlib.net/) | 1.3.2 (PS5), 1.3.1 (native) | PS5, Windows, Android | zlib License |
| [bzip2](https://sourceware.org/bzip2/) | from the PS5 SDK; 1.0.8 (native) | PS5, Windows, Android extractors | bzip2 License (BSD-style) |
| [libpsl](https://github.com/rockdaboot/libpsl) | 0.21.5 | PS5 | MIT |
| [GNU libiconv](https://www.gnu.org/software/libiconv/) | from the PS5 SDK | PS5 | LGPL-2.1-or-later |
| [Mozilla CA certificate list](https://curl.se/docs/caextract.html) | 2026-10-20 | PS5 | MPL-2.0 |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.18.3 | PS5, Windows | MIT |
| [JSON for Modern C++ (nlohmann/json)](https://github.com/nlohmann/json) | 3.11.3 | PS5, Windows, Android | MIT |
| [webview](https://github.com/webview/webview) | 0.12.0 | Windows | MIT |
| [Microsoft Edge WebView2 SDK](https://www.nuget.org/packages/Microsoft.Web.WebView2) | 1.0.1150.38 | Windows | WebView2 SDK license (BSD-3-Clause style) |
| [AndroidX Core](https://developer.android.com/jetpack/androidx/releases/core) | 1.16.0 | Android | Apache-2.0 |
| [AndroidX WebKit](https://developer.android.com/jetpack/androidx/releases/webkit) | 1.12.1 | Android | Apache-2.0 |
| [Karla font](https://github.com/google/fonts/tree/main/ofl/karla) | Google Fonts `130fcfb` | all | SIL Open Font License 1.1 |
| [Font Awesome Free](https://fontawesome.com/) (solid icon font) | 6.7.2 | all | Font: SIL OFL 1.1; icons: CC BY 4.0 |
| [QR Code Generator (kazuhikoarase)](https://github.com/kazuhikoarase/qrcode-generator) | 1.4.4 | all | MIT |

The Karla, Font Awesome, QR Code Generator and (Windows) WebView2 SDK license texts are bundled with every build next to the files they cover. The full license texts of the other components are at the links above and in each component's source package.

Native extraction statically links SHA-256-verified libarchive, zlib, bzip2, liblzma and Zstandard sources fetched only during the build. It never downloads extraction tools or dependencies at runtime. Their notices are generated in `extraction-licenses` for packaging and copied into the Windows web assets; Android packaging uses the same generated license directory.

The PS5 build statically links the GPL-3.0-or-later ps5-payload-sdk, so `ArchiveFetch.elf` (also carried inside the Android APK for Install/Update) is distributed under the GPL-3.0-or-later; its complete source is this repository. The LGPL-2.1-or-later GNU libiconv is linked statically into the same ELF; its source is available from the link above, and this repository's source and build files let you relink the ELF against a modified version.

Some of the console behavior was learned by studying other GPL-3.0 PS5 homebrew projects, such as ps5upload and ShadowMount+. No code from them is included.
