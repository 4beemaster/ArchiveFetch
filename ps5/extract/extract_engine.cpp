#include "extract_engine.hpp"
#include "remove_tree.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <memory>
#include <set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <vector>
#else
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace archivefetch_extract {
namespace {

constexpr unsigned long long space_margin = 64ull << 20;
constexpr std::size_t max_relative_path = 1000;

#ifdef _WIN32
std::wstring wide_path(const std::string& path) {
    return std::filesystem::u8path(path).lexically_normal().make_preferred().wstring();
}

struct stat { unsigned st_mode = 0; };
constexpr unsigned directory_mode = 1;
#ifdef S_ISDIR
#undef S_ISDIR
#endif
bool S_ISDIR(unsigned mode) { return mode == directory_mode; }
int lstat(const char* path, struct stat* info) {
    const auto attributes = GetFileAttributesW(wide_path(path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto code = GetLastError();
        errno = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES;
        return -1;
    }
    info->st_mode = (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) ? directory_mode : 0;
    return 0;
}
int mkdir(const char* path, int) {
    if (CreateDirectoryW(wide_path(path).c_str(), nullptr)) return 0;
    errno = GetLastError() == ERROR_ALREADY_EXISTS ? EEXIST : EACCES;
    return -1;
}
int getpid() { return static_cast<int>(GetCurrentProcessId()); }
int unlink(const char* path) { return DeleteFileW(wide_path(path).c_str()) ? 0 : -1; }
int open(const char* path, int, int) {
    const auto handle = CreateFileW(wide_path(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { errno = EACCES; return -1; }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_BINARY);
    if (fd < 0) CloseHandle(handle);
    return fd;
}
using ssize_t = std::int64_t;
ssize_t pwrite(int fd, const char* data, size_t size, std::int64_t offset) {
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    LARGE_INTEGER position; position.QuadPart = offset;
    DWORD written = 0;
    if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) ||
        !WriteFile(handle, data, static_cast<DWORD>(std::min<size_t>(size, 1u << 30)), &written, nullptr)) {
        errno = GetLastError() == ERROR_DISK_FULL ? ENOSPC : EIO;
        return -1;
    }
    return written;
}
int ftruncate(int fd, std::int64_t size) { return _chsize_s(fd, size) == 0 ? 0 : -1; }
int close(int fd) { return _close(fd); }
int rename(const char* from, const char* to) {
    // No MOVEFILE_REPLACE_EXISTING: case-folding collisions must never overwrite.
    if (MoveFileExW(wide_path(from).c_str(), wide_path(to).c_str(), 0)) return 0;
    errno = EACCES;
    return -1;
}

class DirectoryGuards {
    struct Guard { HANDLE handle; bool owned; BY_HANDLE_FILE_INFORMATION info; };
    std::vector<Guard> handles_;
    std::vector<std::wstring> files_;
    std::set<std::wstring> paths_;
public:
    bool failed = false;
    ~DirectoryGuards() { release(); }
    bool pin(const std::string& path, bool owned = false) {
        const auto wide = wide_path(path);
        if (paths_.count(wide)) return true;
        const auto handle = CreateFileW(wide.c_str(), GENERIC_READ | (owned ? DELETE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            // A differently cased alias may already have an owned, delete-locked
            // handle. Compare file IDs, not strings: NTFS can also be case-sensitive.
            const auto probe = CreateFileW(wide.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            BY_HANDLE_FILE_INFORMATION info{};
            if (probe != INVALID_HANDLE_VALUE) {
                const bool valid = GetFileInformationByHandle(probe, &info) &&
                    (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
                CloseHandle(probe);
                if (valid) for (const auto& guard : handles_) {
                    if (guard.info.dwVolumeSerialNumber == info.dwVolumeSerialNumber &&
                        guard.info.nFileIndexHigh == info.nFileIndexHigh && guard.info.nFileIndexLow == info.nFileIndexLow) {
                        paths_.insert(wide);
                        return true;
                    }
                }
            }
            failed = true; return false;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) { CloseHandle(handle); failed = true; return false; }
        paths_.insert(wide);
        handles_.push_back({handle, owned, info});
        return true;
    }
    void file(const std::string& path) { files_.push_back(wide_path(path)); }
    bool rollback() {
        bool removed = true;
        for (const auto& file : files_)
            if (!DeleteFileW(file.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) removed = false;
        for (auto it = handles_.rbegin(); it != handles_.rend(); ++it) {
            if (!it->owned || it->handle == INVALID_HANDLE_VALUE) continue;
            FILE_DISPOSITION_INFO disposition{TRUE};
            if (!SetFileInformationByHandle(it->handle, FileDispositionInfo, &disposition, sizeof(disposition))) removed = false;
            CloseHandle(it->handle);
            it->handle = INVALID_HANDLE_VALUE;
        }
        return removed;
    }
    void release() {
        for (const auto& guard : handles_) if (guard.handle != INVALID_HANDLE_VALUE) CloseHandle(guard.handle);
        handles_.clear(); paths_.clear();
    }
};
#else
struct DirectoryGuards {
    bool failed = false;
    bool pin(const std::string&, bool = false) { return true; }
    void file(const std::string&) {}
    void release() {}
};
#endif

unsigned long long free_bytes(const std::string& folder) {
#ifdef _WIN32
    ULARGE_INTEGER bytes{};
    return GetDiskFreeSpaceExW(wide_path(folder).c_str(), &bytes, nullptr, nullptr) ? bytes.QuadPart : 0;
#else
    struct statvfs info {};
    return statvfs(folder.c_str(), &info) == 0 ? static_cast<unsigned long long>(info.f_bavail) * info.f_frsize : ~0ull;
#endif
}

// Creates each missing folder below root; anything in the way that is not a real folder refuses the entry.
bool make_folders(const std::string& root, const std::string& relative, DirectoryGuards& guards) {
    std::string path = root;
    std::size_t start = 0;
    while (start < relative.size()) {
        auto end = relative.find('/', start);
        if (end == std::string::npos) end = relative.size();
        path += '/';
        path += relative.substr(start, end - start);
        bool owned = false;
        struct stat existing {};
        if (lstat(path.c_str(), &existing) == 0) {
            if (!S_ISDIR(existing.st_mode)) return false;
        } else {
            if (mkdir(path.c_str(), 0777) == 0) owned = true;
            else if (errno != EEXIST) {
#ifdef _WIN32
                guards.failed = true;
#endif
                return false;
            }
        }
        if (!guards.pin(path, owned)) return false;
        start = end + 1;
    }
    return true;
}

std::string parent_of(const std::string& relative) {
    const auto slash = relative.find_last_of('/');
    return slash == std::string::npos ? std::string{} : relative.substr(0, slash);
}

std::string reader_error(archive* reader) {
    const char* text = archive_error_string(reader);
    return text && *text ? text : "the archive is damaged or not supported";
}

}

std::string safe_entry_path(const std::string& raw) {
    std::string name = raw;
    for (auto& character : name) if (character == '\\') character = '/';
    if (name.empty() || name[0] == '/') return {};
    if (name.size() >= 2 && std::isalpha(static_cast<unsigned char>(name[0])) && name[1] == ':') return {};
    std::string result;
    std::size_t start = 0;
    while (start <= name.size()) {
        auto end = name.find('/', start);
        if (end == std::string::npos) end = name.size();
        auto part = name.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".") continue;
        if (part == ".." || part.size() > 255) return {};
#ifdef _WIN32
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, part.data(), static_cast<int>(part.size()), nullptr, 0)) return {};
        if (part.find('\0') != std::string::npos) return {};
        auto base = part.substr(0, part.find('.'));
        while (!base.empty() && (base.back() == ' ' || base.back() == '.')) base.pop_back();
        for (auto& character : base) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" || base == "CONIN$" || base == "CONOUT$" ||
            (base.size() == 4 && (base.substr(0, 3) == "COM" || base.substr(0, 3) == "LPT") &&
                (base[3] >= '0' && base[3] <= '9')) ||
            ((base.substr(0, 3) == "COM" || base.substr(0, 3) == "LPT") &&
                (base.substr(3) == "\xc2\xb9" || base.substr(3) == "\xc2\xb2" || base.substr(3) == "\xc2\xb3"))) return {};
#endif
        // Characters an exFAT drive cannot hold become '_' everywhere, so a folder can be copied to one later.
        for (auto& character : part) {
            const auto code = static_cast<unsigned char>(character);
            if (code < 0x20 || code == 0x7f || std::strchr("\\:*?\"<>|", character)) character = '_';
        }
        for (auto index = part.size(); index && (part[index - 1] == '.' || part[index - 1] == ' '); --index) part[index - 1] = '_';
        if (!result.empty()) result += '/';
        result += part;
    }
    return result.size() > max_relative_path ? std::string{} : result;
}

Result extract_archive(const Options& options, Progress& progress, std::string& error) {
    progress = {};
    error.clear();
    DirectoryGuards guards;
#ifdef _WIN32
    const auto destination = std::filesystem::absolute(std::filesystem::u8path(options.destination));
    for (auto parent = destination.parent_path(); !parent.empty();) {
        if (!guards.pin(parent.u8string())) { error = "destination ancestry is not a real accessible folder"; return Result::Failed; }
        const auto next = parent.parent_path();
        if (next == parent) break;
        parent = next;
    }
#endif
    struct stat existing {};
    if (lstat(options.destination.c_str(), &existing) == 0) {
        error = "the folder " + std::filesystem::path(options.destination).filename().string() + " already exists";
        return Result::Failed;
    }
    if (errno != ENOENT) { error = "cannot inspect destination"; return Result::Failed; }
    archive* reader = archive_read_new();
    if (!reader) {
        error = "out of memory";
        return Result::Failed;
    }
    const std::unique_ptr<archive, decltype(&archive_read_free)> reader_owner(reader, archive_read_free);
#if defined(_WIN32) || defined(__ANDROID__)
    archive_read_support_filter_none(reader);
    archive_read_support_filter_gzip(reader);
    archive_read_support_filter_bzip2(reader);
    archive_read_support_filter_xz(reader);
    archive_read_support_filter_zstd(reader);
#else
    archive_read_support_filter_all(reader);
#endif
    archive_read_support_format_zip(reader);
    archive_read_support_format_7zip(reader);
    archive_read_support_format_rar(reader);
    archive_read_support_format_rar5(reader);
    archive_read_support_format_tar(reader);
#ifdef _WIN32
    const int opened = archive_read_open_filename_w(reader, wide_path(options.archive).c_str(), 1 << 20);
#else
    const int opened = archive_read_open_filename(reader, options.archive.c_str(), 1 << 20);
#endif
    if (opened != ARCHIVE_OK) {
        error = "cannot open the archive: " + reader_error(reader);
        return Result::Failed;
    }
    if (mkdir(options.destination.c_str(), 0777) != 0) {
        error = std::string("cannot create the folder: ") + std::strerror(errno);
        return Result::Failed;
    }
    if (!guards.pin(options.destination, true)) {
        error = "cannot protect extraction folder";
#ifdef _WIN32
        if (!RemoveDirectoryW(wide_path(options.destination).c_str())) error += "; cannot remove the newly created folder";
#endif
        return Result::Failed;
    }
    struct OwnedOutput {
        const Options& options;
        DirectoryGuards& guards;
        std::string& error;
        bool keep = false;
        ~OwnedOutput() {
            if (keep) return;
#ifdef _WIN32
            if (!guards.rollback()) error += "; cannot remove all owned incomplete output";
#else
            std::error_code failure;
            if (!archive_catalog::remove_tree(std::filesystem::u8path(options.destination), failure))
                error += "; cannot remove incomplete folder: " + failure.message();
#endif
        }
    } output{options, guards, error};
    if (options.started) options.started();

    Result result = Result::Done;
    const auto fail = [&](Result outcome, std::string message) {
        result = outcome;
        error = std::move(message);
    };
    const auto stopped = [&] { return options.cancelled && options.cancelled(); };
    const auto report = [&] {
        progress.consumed = static_cast<std::uint64_t>(archive_filter_bytes(reader, -1));
        if (options.progress) options.progress(progress);
    };
    unsigned sequence = 0;
    for (;;) {
        if (stopped()) { fail(Result::Cancelled, "stopped"); break; }
        archive_entry* entry = nullptr;
        const int header = archive_read_next_header(reader, &entry);
        if (header == ARCHIVE_EOF) break;
        if (header < ARCHIVE_WARN) { fail(Result::Failed, reader_error(reader)); break; }
        if (archive_entry_is_encrypted(entry)) { fail(Result::Failed, "password-protected archives are not supported"); break; }
        const char* raw = archive_entry_pathname_utf8(entry);
        if (!raw) raw = archive_entry_pathname(entry);
        const auto relative = safe_entry_path(raw ? raw : "");
        progress.current = relative;
        // Unread data of a skipped entry is skipped by the next archive_read_next_header.
        if (relative.empty()) { ++progress.unsafe; continue; }
        const auto type = archive_entry_filetype(entry);
        if (archive_entry_hardlink(entry) || archive_entry_symlink(entry) || (type != AE_IFREG && type != AE_IFDIR)) { ++progress.links; continue; }
        if (type == AE_IFDIR) {
            if (!make_folders(options.destination, relative, guards)) ++progress.unsafe;
            if (guards.failed) { fail(Result::Failed, "cannot securely create output directory " + relative); break; }
            continue;
        }
        const auto parent = parent_of(relative);
        if (!make_folders(options.destination, parent, guards)) {
            if (guards.failed) { fail(Result::Failed, "cannot securely create output directory " + parent); break; }
            ++progress.unsafe; continue;
        }
        const auto final_path = options.destination + "/" + relative;
#ifdef _WIN32
        if (lstat(final_path.c_str(), &existing) == 0) { ++progress.unsafe; continue; }
        if (errno != ENOENT) { fail(Result::Failed, "cannot inspect " + relative); break; }
#else
        if (lstat(final_path.c_str(), &existing) == 0 && S_ISDIR(existing.st_mode)) { ++progress.unsafe; continue; }
#endif
        const auto folder = parent.empty() ? options.destination : options.destination + "/" + parent;
        if (archive_entry_size_is_set(entry)) {
            const auto size = static_cast<unsigned long long>(archive_entry_size(entry));
            if (archive_entry_size(entry) < 0 || size > std::numeric_limits<unsigned long long>::max() - space_margin ||
                free_bytes(folder) < size + space_margin) { fail(Result::Failed, "not enough free space for " + relative); break; }
        }

        // Written beside its final name and renamed into place: a rename across drives panics the PS5 kernel.
        const auto temporary = folder + "/.archivefetch-x-" + std::to_string(getpid()) + "-" + std::to_string(++sequence) + ".part";
        const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0666);
        if (fd < 0) { fail(Result::Failed, "cannot write " + relative + ": " + std::strerror(errno)); break; }
        struct PendingFile {
            int fd;
            const std::string& path;
            bool present = true;
            ~PendingFile() { if (fd >= 0) close(fd); if (present) unlink(path.c_str()); }
        } pending{fd, temporary};
        bool written = true;
        const void* block = nullptr;
        size_t size = 0;
        la_int64_t offset = 0;
        int read = ARCHIVE_OK;
        while ((read = archive_read_data_block(reader, &block, &size, &offset)) == ARCHIVE_OK || read == ARCHIVE_WARN) {
            if (stopped()) { fail(Result::Cancelled, "stopped"); written = false; break; }
            const auto* data = static_cast<const char*>(block);
            for (size_t done = 0; done < size;) {
                if (offset < 0 || static_cast<std::uint64_t>(offset) > static_cast<std::uint64_t>(INT64_MAX) - size) {
                    fail(Result::Failed, "invalid archive data offset"); written = false; break;
                }
                const ssize_t count = pwrite(fd, data + done, size - done, offset + done);
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) {
                    fail(Result::Failed, errno == ENOSPC ? "the drive is full" : "cannot write " + relative + ": " + std::strerror(errno));
                    written = false;
                    break;
                }
                done += static_cast<size_t>(count);
            }
            if (!written) break;
            progress.bytes += size;
            report();
        }
        if (written && read != ARCHIVE_EOF) { fail(Result::Failed, reader_error(reader)); written = false; }
        // A sparse entry can end in a hole that pwrite never reached.
        if (written && archive_entry_size_is_set(entry) && ftruncate(fd, archive_entry_size(entry)) != 0) {
            fail(Result::Failed, "cannot write " + relative + ": " + std::strerror(errno));
            written = false;
        }
        const auto closed = close(fd);
        pending.fd = -1;
        if (closed != 0 && written) {
            fail(Result::Failed, errno == ENOSPC ? "the drive is full" : "cannot write " + relative + ": " + std::strerror(errno));
            written = false;
        }
        if (!written) break;
        if (archive_entry_mtime_is_set(entry)) {
#ifdef _WIN32
            const auto handle = CreateFileW(wide_path(temporary).c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                const auto seconds = archive_entry_mtime(entry);
                if (seconds >= -11644473600ll && seconds <= 910692730085ll) {
                    ULARGE_INTEGER ticks{}; ticks.QuadPart = static_cast<std::uint64_t>(seconds + 11644473600ll) * 10000000ull;
                    FILETIME time{ticks.LowPart, ticks.HighPart};
                    SetFileTime(handle, nullptr, &time, &time);
                }
                CloseHandle(handle);
            }
#else
            const timeval times[2] = {{static_cast<time_t>(archive_entry_mtime(entry)), 0}, {static_cast<time_t>(archive_entry_mtime(entry)), 0}};
            utimes(temporary.c_str(), times);
#endif
        }
        if (rename(temporary.c_str(), final_path.c_str()) != 0) {
            fail(Result::Failed, "cannot finish " + relative + ": " + std::strerror(errno));
            break;
        }
        pending.present = false;
        ++progress.files;
        guards.file(final_path);
        report();
    }
    output.keep = result == Result::Done;
    return result;
}

}
