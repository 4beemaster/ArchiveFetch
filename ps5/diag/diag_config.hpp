#pragma once

namespace archivefetch_diag {

inline constexpr const char* version = ARCHIVEFETCH_VERSION;
inline constexpr const char* process_name = "ArchFtch.elf";
inline constexpr const char* title_id = "ARFT00002";
inline constexpr int http_port = 8890;
inline constexpr const char* log_directory = "/data/archivefetch/logs";
inline constexpr const char* log_path = "/data/archivefetch/logs/run.log";
inline constexpr const char* log_previous_path = "/data/archivefetch/logs/run.previous.log";
// At startup a larger log is moved to log_previous_path, replacing the older one.
inline constexpr long long log_rotate_bytes = 1024 * 1024;
// Same data folder as the earlier ArchiveFetch builds, so their saved sources and settings load.
inline constexpr const char* app_data_root = "/data/archivefetch";

}
