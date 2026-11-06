#pragma once

#include <cstdint>
#include <filesystem>
#include <system_error>

#ifndef _WIN32
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#endif

namespace archive_catalog {

#ifndef _WIN32
// Names are read first and the folder closed again, so deep trees never hold many folders open.
inline bool folder_names(const std::filesystem::path& path, std::vector<std::string>& names) {
    DIR* folder = opendir(path.c_str());
    if (!folder) return false;
    while (const dirent* entry = readdir(folder))
        if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) names.emplace_back(entry->d_name);
    closedir(folder);
    return true;
}
#endif

// Bytes in the regular files of a file or folder, never following links; unreadable parts count as 0.
inline std::uintmax_t tree_size(const std::filesystem::path& path) {
#ifdef _WIN32
    std::error_code failure;
    const auto kind = std::filesystem::symlink_status(path, failure);
    if (failure) return 0;
    if (std::filesystem::is_regular_file(kind)) { const auto size = std::filesystem::file_size(path, failure); return failure ? 0 : size; }
    if (!std::filesystem::is_directory(kind)) return 0;
    std::uintmax_t total = 0;
    for (std::filesystem::recursive_directory_iterator entry(path, failure), end; !failure && entry != end; entry.increment(failure)) {
        std::error_code ignored;
        if (std::filesystem::is_regular_file(entry->symlink_status(ignored))) { const auto size = entry->file_size(ignored); if (!ignored) total += size; }
    }
    return total;
#else
    struct stat info {};
    if (lstat(path.c_str(), &info) != 0) return 0;
    if (S_ISREG(info.st_mode)) return static_cast<std::uintmax_t>(info.st_size);
    std::vector<std::string> names;
    if (!S_ISDIR(info.st_mode) || !folder_names(path, names)) return 0;
    std::uintmax_t total = 0;
    for (const auto& name : names) total += tree_size(path / name);
    return total;
#endif
}

// Deletes a file or a folder with everything in it, never following links, adding the bytes removed to freed.
// On the PS5, std::filesystem::remove_all fails ("Bad file descriptor"), so POSIX builds walk the folder with plain calls.
inline bool remove_tree(const std::filesystem::path& path, std::error_code& failure, std::uintmax_t* freed = nullptr) {
    failure.clear();
#ifdef _WIN32
    const auto size = freed ? tree_size(path) : 0;
    std::filesystem::remove_all(path, failure);
    if (!failure && freed) *freed += size;
    return !failure;
#else
    const auto fail = [&failure] { failure.assign(errno, std::generic_category()); return false; };
    struct stat info {};
    if (lstat(path.c_str(), &info) != 0) return errno == ENOENT ? true : fail();
    if (!S_ISDIR(info.st_mode)) {
        if (unlink(path.c_str()) != 0) return errno == ENOENT ? true : fail();
        if (freed && S_ISREG(info.st_mode)) *freed += static_cast<std::uintmax_t>(info.st_size);
        return true;
    }
    std::vector<std::string> names;
    if (!folder_names(path, names)) return fail();
    for (const auto& name : names) if (!remove_tree(path / name, failure, freed)) return false;
    return rmdir(path.c_str()) == 0 || errno == ENOENT ? true : fail();
#endif
}

}
