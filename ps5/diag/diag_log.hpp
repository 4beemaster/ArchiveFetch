#pragma once

#include <initializer_list>

namespace archivefetch_diag {

struct Field {
    enum class Kind { Text, Integer, Hex };
    const char* key;
    Kind kind;
    const char* text;
    long long integer;
};

inline Field field(const char* key, const char* value) { return {key, Field::Kind::Text, value, 0}; }
inline Field field(const char* key, long long value) { return {key, Field::Kind::Integer, nullptr, value}; }
inline Field field(const char* key, int value) { return field(key, static_cast<long long>(value)); }
inline Field field_hex(const char* key, int value) { return {key, Field::Kind::Hex, nullptr, value}; }

// Creates the directory if needed and appends to path; on failure errno is stored in error_out.
bool log_open(const char* directory, const char* path, int* error_out);
// One JSON object per line, fsynced; writes to stderr while no log file is open.
void log_event(const char* event, std::initializer_list<Field> details = {});
void log_close();
// Size of a file in bytes, or 0 if it does not exist.
long long log_size(const char* path);
// Moves path to previous when it exceeds limit; returns the moved size, 0 if nothing moved, -1 on failure.
long long log_rotate(const char* path, const char* previous, long long limit);
// Empties the open log; later lines start from the beginning of the file.
bool log_clear();
// Logs a fatal signal with async-signal-safe writes, then exits so the port is released.
void log_install_crash_handler();

struct NotifyResult {
    bool available;
    int result;
};
NotifyResult notify(const char* text);

}
