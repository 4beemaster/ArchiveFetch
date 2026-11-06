#!/bin/sh
# Native tests for the PS5 extractor, run in WSL: the unpacking rules (host_test.cpp), then ArchiveFetch's side
# against the real extractor program started by a fake ELF loader (host_protocol_test.cpp).
# Uses the PS5 SDK's libarchive headers with the system libarchive.so.13 and clang++-18. Nothing runs on the console.
set -e
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
sdk=/opt/ps5-payload-sdk/target/user/homebrew/include
libarchive=/usr/lib/x86_64-linux-gnu/libarchive.so.13
json=build/_deps/web_json-src/include
mkdir -p "$out/include" "$out/data"
cp "$sdk/archive.h" "$sdk/archive_entry.h" "$out/include/"
compile="clang++-18 -std=c++17 -O1 -Wall -Wextra -DARCHIVEFETCH_VERSION=\"$(cat VERSION)\" -I$out/include -Ips5/extract -Ips5/diag -Isrc -I$json"
paths="-DARCHIVEFETCH_EXTRACT_ROOT=\"$out/data\" -DARCHIVEFETCH_LOADER_PORT=19021"

$compile -o "$out/engine_test" ps5/extract/host_test.cpp ps5/extract/extract_engine.cpp "$libarchive"
"$out/engine_test"

$compile $paths -DARCHIVEFETCH_EXTRACT_TEST_DELAY_MS=100 -o "$out/extractor" ps5/extract/extract_main.cpp ps5/extract/extract_engine.cpp ps5/diag/diag_log.cpp "$libarchive"
{ echo 'const unsigned char extractor_elf[] = {'; xxd -i < "$out/extractor"; echo '};'; } > "$out/extractor_elf.hpp"
$compile $paths -I"$out" -o "$out/protocol_test" ps5/extract/host_protocol_test.cpp ps5/diag/extract_runner.cpp ps5/diag/diag_log.cpp "$libarchive" -lpthread
"$out/protocol_test"
rm -rf "$out"
