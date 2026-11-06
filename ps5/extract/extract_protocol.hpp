#pragma once

// Files through which ArchiveFetch and its extractor (ArchFtchX.elf) talk. Overridable for a native test run.
#ifndef ARCHIVEFETCH_EXTRACT_ROOT
#define ARCHIVEFETCH_EXTRACT_ROOT "/data/archivefetch"
#endif
#ifndef ARCHIVEFETCH_LOADER_PORT
#define ARCHIVEFETCH_LOADER_PORT 9021
#endif

namespace archivefetch_extract {

inline constexpr const char* data_root = ARCHIVEFETCH_EXTRACT_ROOT;
inline constexpr const char* work_directory = ARCHIVEFETCH_EXTRACT_ROOT "/extract";
// Written by ArchiveFetch, read and deleted by the extractor at start.
inline constexpr const char* request_path = ARCHIVEFETCH_EXTRACT_ROOT "/extract/request.json";
// Rewritten by the extractor (temporary file, then rename) at most every 250 ms and once at the end.
inline constexpr const char* status_path = ARCHIVEFETCH_EXTRACT_ROOT "/extract/status.json";
// Created by ArchiveFetch to ask the extractor to stop.
inline constexpr const char* cancel_path = ARCHIVEFETCH_EXTRACT_ROOT "/extract/cancel";
inline constexpr const char* log_directory = ARCHIVEFETCH_EXTRACT_ROOT "/logs";
inline constexpr const char* log_path = ARCHIVEFETCH_EXTRACT_ROOT "/logs/extract.log";
inline constexpr const char* log_previous_path = ARCHIVEFETCH_EXTRACT_ROOT "/logs/extract.previous.log";
inline constexpr int loader_port = ARCHIVEFETCH_LOADER_PORT;

}
