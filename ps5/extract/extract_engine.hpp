#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace archivefetch_extract {

struct Progress {
    std::uint64_t files = 0, bytes = 0, unsafe = 0, links = 0;
    // Bytes of the archive read so far, for a percentage.
    std::uint64_t consumed = 0;
    std::string current;
};

enum class Result { Done, Failed, Cancelled };

struct Options {
    std::string archive, destination;
    std::function<bool()> cancelled;
    // Called once the destination folder exists, then after every block and file; the caller throttles.
    std::function<void()> started;
    std::function<void(const Progress&)> progress;
};

// Single-threaded. Refuses a destination that already exists; removes the destination it made after a failure or stop.
Result extract_archive(const Options& options, Progress& progress, std::string& error);

// Archive entry name made safe for a folder on the PS5 or an exFAT drive; empty when the entry must be skipped.
std::string safe_entry_path(const std::string& name);

}
