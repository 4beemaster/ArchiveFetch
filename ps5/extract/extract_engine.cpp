#include "extract_engine.hpp"
#include "remove_tree.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

namespace archivefetch_extract {
namespace {

constexpr unsigned long long space_margin = 64ull << 20;
constexpr std::size_t max_relative_path = 1000;

unsigned long long free_bytes(const std::string& folder) {
    struct statvfs info {};
    return statvfs(folder.c_str(), &info) == 0 ? static_cast<unsigned long long>(info.f_bavail) * info.f_frsize : ~0ull;
}

// Creates each missing folder below root; anything in the way that is not a real folder refuses the entry.
bool make_folders(const std::string& root, const std::string& relative) {
    std::string path = root;
    std::size_t start = 0;
    while (start < relative.size()) {
        auto end = relative.find('/', start);
        if (end == std::string::npos) end = relative.size();
        path += '/';
        path += relative.substr(start, end - start);
        struct stat existing {};
        if (lstat(path.c_str(), &existing) == 0) {
            if (!S_ISDIR(existing.st_mode)) return false;
        } else if (mkdir(path.c_str(), 0777) != 0 && errno != EEXIST) {
            return false;
        }
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
        // Characters an exFAT drive cannot hold become '_' everywhere, so a folder can be copied to one later.
        for (auto& character : part) {
            const auto code = static_cast<unsigned char>(character);
            if (code < 0x20 || code == 0x7f || std::strchr("\\:*?\"<>|", character)) character = '_';
        }
        if (part.back() == '.' || part.back() == ' ') part.back() = '_';
        if (!result.empty()) result += '/';
        result += part;
    }
    return result.size() > max_relative_path ? std::string{} : result;
}

Result extract_archive(const Options& options, Progress& progress, std::string& error) {
    struct stat existing {};
    if (lstat(options.destination.c_str(), &existing) == 0) {
        error = "the folder " + std::filesystem::path(options.destination).filename().string() + " already exists";
        return Result::Failed;
    }
    archive* reader = archive_read_new();
    if (!reader) {
        error = "out of memory";
        return Result::Failed;
    }
    archive_read_support_filter_all(reader);
    archive_read_support_format_zip(reader);
    archive_read_support_format_7zip(reader);
    archive_read_support_format_rar(reader);
    archive_read_support_format_rar5(reader);
    archive_read_support_format_tar(reader);
    if (archive_read_open_filename(reader, options.archive.c_str(), 1 << 20) != ARCHIVE_OK) {
        error = "cannot open the archive: " + reader_error(reader);
        archive_read_free(reader);
        return Result::Failed;
    }
    if (mkdir(options.destination.c_str(), 0777) != 0) {
        error = std::string("cannot create the folder: ") + std::strerror(errno);
        archive_read_free(reader);
        return Result::Failed;
    }
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
            if (!make_folders(options.destination, relative)) ++progress.unsafe;
            continue;
        }
        const auto parent = parent_of(relative);
        if (!make_folders(options.destination, parent)) { ++progress.unsafe; continue; }
        const auto final_path = options.destination + "/" + relative;
        if (lstat(final_path.c_str(), &existing) == 0 && S_ISDIR(existing.st_mode)) { ++progress.unsafe; continue; }
        const auto folder = parent.empty() ? options.destination : options.destination + "/" + parent;
        if (archive_entry_size_is_set(entry)) {
            const auto size = static_cast<unsigned long long>(archive_entry_size(entry));
            if (free_bytes(folder) < size + space_margin) { fail(Result::Failed, "not enough free space for " + relative); break; }
        }

        // Written beside its final name and renamed into place: a rename across drives panics the PS5 kernel.
        const auto temporary = folder + "/.archivefetch-x-" + std::to_string(getpid()) + "-" + std::to_string(++sequence) + ".part";
        const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0666);
        if (fd < 0) { fail(Result::Failed, "cannot write " + relative + ": " + std::strerror(errno)); break; }
        bool written = true;
        const void* block = nullptr;
        size_t size = 0;
        la_int64_t offset = 0;
        int read = ARCHIVE_OK;
        while ((read = archive_read_data_block(reader, &block, &size, &offset)) == ARCHIVE_OK || read == ARCHIVE_WARN) {
            if (stopped()) { fail(Result::Cancelled, "stopped"); written = false; break; }
            const auto* data = static_cast<const char*>(block);
            for (size_t done = 0; done < size;) {
                const ssize_t count = pwrite(fd, data + done, size - done, static_cast<off_t>(offset + done));
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
        if (written && archive_entry_size_is_set(entry) && ftruncate(fd, static_cast<off_t>(archive_entry_size(entry))) != 0) {
            fail(Result::Failed, "cannot write " + relative + ": " + std::strerror(errno));
            written = false;
        }
        if (close(fd) != 0 && written) {
            fail(Result::Failed, errno == ENOSPC ? "the drive is full" : "cannot write " + relative + ": " + std::strerror(errno));
            written = false;
        }
        if (!written) { unlink(temporary.c_str()); break; }
        if (archive_entry_mtime_is_set(entry)) {
            const timeval times[2] = {{static_cast<time_t>(archive_entry_mtime(entry)), 0}, {static_cast<time_t>(archive_entry_mtime(entry)), 0}};
            utimes(temporary.c_str(), times);
        }
        if (rename(temporary.c_str(), final_path.c_str()) != 0) {
            fail(Result::Failed, "cannot finish " + relative + ": " + std::strerror(errno));
            unlink(temporary.c_str());
            break;
        }
        ++progress.files;
        report();
    }
    archive_read_free(reader);
    if (result != Result::Done) {
        std::error_code ignored;
        archive_catalog::remove_tree(options.destination, ignored);
    }
    return result;
}

}
