#include "native_extraction.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace archive_catalog;
namespace {
struct Entry { std::string name, data; int type = AE_IFREG; std::string link; };
void check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
void make(const fs::path& path, const char* format, const char* filter, const std::vector<Entry>& entries, bool encrypted = false) {
    auto* writer = archive_write_new();
    check(writer != nullptr, "writer allocation");
    check(archive_write_set_format_by_name(writer, format) == ARCHIVE_OK, "writer format");
    if (filter) check(archive_write_add_filter_by_name(writer, filter) == ARCHIVE_OK, "writer filter");
    if (encrypted) {
        check(archive_write_set_passphrase(writer, "secret") == ARCHIVE_OK, "encryption passphrase");
        check(archive_write_set_options(writer, "zip:encryption=zipcrypt") == ARCHIVE_OK, "encrypted fixture");
    }
#ifdef _WIN32
    check(archive_write_open_filename_w(writer, path.c_str()) == ARCHIVE_OK, "writer open");
#else
    check(archive_write_open_filename(writer, path.c_str()) == ARCHIVE_OK, "writer open");
#endif
    for (const auto& item : entries) {
        auto* entry = archive_entry_new();
        archive_entry_set_pathname_utf8(entry, item.name.c_str());
        archive_entry_set_filetype(entry, item.type);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(entry, item.type == AE_IFREG ? item.data.size() : 0);
        if (item.type == AE_IFLNK) archive_entry_set_symlink(entry, item.link.c_str());
        check(archive_write_header(writer, entry) == ARCHIVE_OK, "writer header " + item.name);
        if (!item.data.empty()) check(archive_write_data(writer, item.data.data(), item.data.size()) == static_cast<la_ssize_t>(item.data.size()), "writer data");
        archive_entry_free(entry);
    }
    check(archive_write_close(writer) == ARCHIVE_OK, "writer close");
    archive_write_free(writer);
}
ExtractOutcome run(const fs::path& archive, const fs::path& folder, const std::function<bool()>& stop = {}) {
    std::uint64_t previous = 0;
    return native_extract(archive, folder, stop, {}, [&](std::uint64_t current, std::uint64_t total) {
        check(current >= previous && current <= total, "bounded monotonic progress");
        previous = current;
    });
}
void decode_fixture(const fs::path& source, const fs::path& destination) {
    std::ifstream input(source);
    check(input.good(), "pinned upstream fixture exists: " + source.filename().string());
    std::ofstream output(destination, std::ios::binary);
    std::string line;
    bool started = false, ended = false;
    while (std::getline(input, line)) {
        if (!started) { started = line.rfind("begin ", 0) == 0; continue; }
        if (line == "end" || line == "end\r") { ended = true; break; }
        if (line.empty()) continue;
        const auto count = (static_cast<unsigned char>(line[0]) - 32u) & 63u;
        check(count <= 45 && line.size() >= 1 + ((count + 2) / 3) * 4, "valid uuencoded fixture");
        unsigned bits = 0, available = 0, written = 0;
        for (std::size_t index = 1; index < line.size() && written < count; ++index) {
            bits = (bits << 6) | ((static_cast<unsigned char>(line[index]) - 32u) & 63u);
            available += 6;
            if (available >= 8) { available -= 8; output.put(static_cast<char>(bits >> available)); ++written; }
        }
    }
    check(started && ended && output.good(), "complete fixture decode");
}
#ifdef _WIN32
void junction(const fs::path& path, const fs::path& target) {
    check(fs::create_directory(path), "junction directory");
    const std::wstring substitute = L"\\??\\" + fs::absolute(target).wstring();
    struct MountPoint {
        DWORD tag;
        WORD length, reserved, substitute_offset, substitute_length, print_offset, print_length;
        wchar_t names[2048];
    } buffer{};
    check(substitute.size() + 2 < 2048, "junction target length");
    buffer.tag = IO_REPARSE_TAG_MOUNT_POINT;
    buffer.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    buffer.print_offset = buffer.substitute_length + sizeof(wchar_t);
    buffer.length = static_cast<WORD>(8 + buffer.print_offset + sizeof(wchar_t));
    std::copy(substitute.begin(), substitute.end(), buffer.names);
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    check(handle != INVALID_HANDLE_VALUE, "junction handle");
    DWORD returned = 0;
    const bool made = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &buffer, buffer.length + 8, nullptr, 0, &returned, nullptr) != 0;
    CloseHandle(handle);
    check(made, "junction creation");
}
#endif
}

int main(int argc, char** argv) {
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = getpid();
#endif
    const auto root = fs::temp_directory_path() / ("archivefetch-native-test-" + std::to_string(pid));
    try {
        check(fs::create_directory(root), "isolated test root");
        // All codecs must work without a command-line executable on PATH.
#ifdef _WIN32
        check(_putenv_s("PATH", "") == 0, "clear test process codec search path");
#else
        check(setenv("PATH", "", 1) == 0, "clear test process codec search path");
#endif
        auto* zstd_reader = archive_read_new();
        check(zstd_reader != nullptr, "zstd reader allocation");
        const auto zstd_support = archive_read_support_filter_zstd(zstd_reader);
        archive_read_free(zstd_reader);
        check(zstd_support == ARCHIVE_OK, "zstd must use the native decoder, not an external-program fallback");
        const std::string large(3 * 1024 * 1024 + 17, 'X');
        for (const auto& [format, filter, suffix] : std::vector<std::tuple<const char*, const char*, const char*>>{
            {"zip", nullptr, "zip"}, {"7zip", nullptr, "7z"}, {"pax", nullptr, "tar"},
            {"pax", "gzip", "gz"}, {"pax", "bzip2", "bz2"}, {"pax", "xz", "xz"}, {"pax", "zstd", "tar.zst"}}) {
            const auto source = root / (std::string("archive.") + suffix);
            const auto destination = root / fs::u8path(std::string("out-\xc3\xa9-") + suffix);
            make(source, format, filter, {{"folder/data.bin", large}, {"empty", ""}, {"folder/", "", AE_IFDIR}});
            auto outcome = run(source, destination);
            check(outcome.result == ExtractOutcome::Result::Done, std::string(suffix) + ": " + outcome.error);
            check(read(destination / "folder/data.bin") == large && fs::file_size(destination / "empty") == 0, "format contents");
            check(outcome.files == 2 && outcome.bytes == large.size(), "format counts");
            check(run(source, destination).result == ExtractOutcome::Result::Failed && read(destination / "folder/data.bin") == large, "existing destination preserved");
            std::cout << "ok: " << suffix << '\n';
        }
        check(argc == 2, "upstream fixture directory argument");
        const auto fixtures = fs::u8path(argv[1]);
        for (const auto& fixture : {"test_compat_zstd_1.tar.zst", "test_compat_zstd_2.tar.zst"}) {
            const auto archive = root / fixture;
            decode_fixture(fixtures / (std::string(fixture) + ".uu"), archive);
            const auto folder = root / (std::string(fixture) + "-out");
            const auto outcome = run(archive, folder);
            check(outcome.result == ExtractOutcome::Result::Done && outcome.files == 6, std::string(fixture) + ": " + outcome.error);
            for (const auto& file : {"f1", "f2", "f3", "d1/f1", "d1/f2", "d1/f3"})
                check(fs::is_regular_file(folder / file), "concatenated/skippable Zstd frames preserve every tar member");
            std::cout << "ok: " << fixture << '\n';
        }
        for (const auto& fixture : {"test_read_format_rar.rar", "test_read_format_rar_compress_normal.rar",
            "test_read_format_rar_compress_best.rar", "test_read_format_rar5_stored.rar",
            "test_read_format_rar5_compressed.rar", "test_read_format_rar5_hardlink.rar"}) {
            const auto archive = root / fixture;
            decode_fixture(fixtures / (std::string(fixture) + ".uu"), archive);
            const auto folder = root / (std::string(fixture) + "-out");
            const auto outcome = run(archive, folder);
            check(outcome.result == ExtractOutcome::Result::Done && outcome.files > 0, std::string(fixture) + ": " + outcome.error);
            if (std::string(fixture) == "test_read_format_rar.rar")
                check(read(folder / "test.txt") == "test text document\r\n" && outcome.links > 0, "RAR contents and links");
            if (std::string(fixture) == "test_read_format_rar5_stored.rar")
                check(read(folder / "helloworld.txt") == "hello libarchive test suite!\n", "RAR5 stored contents");
            if (std::string(fixture) == "test_read_format_rar5_compressed.rar")
                check(fs::file_size(folder / "test.bin") == 1200, "RAR5 compressed contents");
            if (std::string(fixture) == "test_read_format_rar5_hardlink.rar")
                check(outcome.links > 0, "RAR5 hardlinks refused");
            std::cout << "ok: " << fixture << '\n';
        }
        for (const auto& fixture : {"test_read_format_rar4_encrypted.rar", "test_read_format_rar5_encrypted.rar"}) {
            const auto archive = root / fixture;
            decode_fixture(fixtures / (std::string(fixture) + ".uu"), archive);
            const auto folder = root / (std::string(fixture) + "-out");
            const auto outcome = run(archive, folder);
            check(outcome.result == ExtractOutcome::Result::Failed && !outcome.error.empty() && !fs::exists(folder), "encrypted RAR rejected");
        }
        const auto source = root / fs::u8path("names-\xc3\xa9.tar");
        make(source, "pax", nullptr, {{"../outside", "bad"}, {"/absolute", "bad"}, {"C:/drive", "bad"},
            {"..\\outside", "bad"}, {"link", "", AE_IFLNK, "../outside"}, {"CON.txt", "bad"}, {"COM1", "bad"},
            {"bad:name", "first"}, {"bad?name", "second"}, {"Case.txt", "first"}, {"case.txt", "second"},
            {"file", "file"}, {"file/child", "bad"}, {"Folder/", "", AE_IFDIR}, {"folder/child", "child"},
            {"utf8-\xc3\xa9.txt", "unicode"}});
        const auto destination = root / "names";
        const auto names = run(source, destination);
        check(names.result == ExtractOutcome::Result::Done, "hostile entries: " + names.error);
        check(!fs::exists(root / "outside") && !fs::exists(destination / "link"), "no traversal or links");
        check(read(destination / fs::u8path("utf8-\xc3\xa9.txt")) == "unicode", "Unicode entry name");
#ifdef _WIN32
        check(!fs::exists(destination / "CON.txt") && !fs::exists(destination / "COM1"), "reserved names refused");
        check(read(destination / "bad_name") == "first" && read(destination / "case.txt") == "first", "collisions never overwrite");
        check(read(destination / "Folder/child") == "child", "case-folded directory aliases share a guard");
        check(names.unsafe == 9 && names.links == 1, "unsafe/link accounting");
        for (const auto& path : {"LPT9.txt", "NUL", "AUX.ext", "CONIN$", "COM\xc2\xb9.txt"})
            check(archivefetch_extract::safe_entry_path(path).empty(), "device alias rejected");
#endif
        bool cancelled = false;
        const auto cancel = root / "cancel";
        const auto stopped = native_extract(root / "archive.zip", cancel, [&] { return cancelled; }, {},
            [&](std::uint64_t consumed, std::uint64_t) { if (consumed) cancelled = true; });
        check(stopped.result == ExtractOutcome::Result::Cancelled && !fs::exists(cancel), "mid-stream cancellation rolls back");
        bool callback_failed = false;
        try {
            native_extract(root / "archive.zip", root / "callback-failure", {}, {},
                [](std::uint64_t, std::uint64_t) { throw std::runtime_error("test callback failure"); });
        } catch (const std::runtime_error& error) {
            callback_failed = std::string(error.what()) == "test callback failure";
        }
        check(callback_failed && !fs::exists(root / "callback-failure"), "callback exceptions close files and roll back owned output");
        make(root / "encrypted.zip", "zip", nullptr, {{"secret", "secret"}}, true);
        const auto locked = run(root / "encrypted.zip", root / "locked");
        check(locked.result == ExtractOutcome::Result::Failed && locked.error.find("password") != std::string::npos &&
            !fs::exists(root / "locked"), "encrypted archive explicitly refused");
        std::ofstream(root / "broken.zip") << "not an archive";
        check(run(root / "broken.zip", root / "broken").result == ExtractOutcome::Result::Failed && !fs::exists(root / "broken"), "invalid archive rollback");
        const auto truncated = read(root / "archive.zip");
        std::ofstream(root / "truncated.zip", std::ios::binary).write(truncated.data(), static_cast<std::streamsize>(truncated.size() / 2));
        check(run(root / "truncated.zip", root / "truncated").result == ExtractOutcome::Result::Failed &&
            !fs::exists(root / "truncated"), "truncated archive rolls back owned files");
        {
            std::array<char, 512> header{};
            std::copy_n("huge.bin", 8, header.data());
            std::copy_n("0000644", 7, header.data() + 100);
            header[124] = static_cast<char>(0x80);
            header[128] = 0x08; // Base-256 tar size: 2^59 bytes, below libarchive's entry limit.
            std::fill_n(header.data() + 148, 8, ' ');
            header[156] = '0';
            std::copy_n("ustar", 5, header.data() + 257);
            unsigned checksum = 0;
            for (const auto byte : header) checksum += static_cast<unsigned char>(byte);
            std::snprintf(header.data() + 148, 8, "%06o", checksum);
            header[155] = ' ';
            std::ofstream huge(root / "space.tar", std::ios::binary);
            huge.write(header.data(), header.size());
            huge.close();
            const auto outcome = run(root / "space.tar", root / "space");
            check(outcome.result == ExtractOutcome::Result::Failed && outcome.error.find("not enough free space") != std::string::npos &&
                !fs::exists(root / "space"), "space preflight rejects huge entry without allocating or writing it: " + outcome.error);
        }
        std::ofstream(root / "protected") << "keep";
        check(run(root / "archive.zip", root / "protected").result == ExtractOutcome::Result::Failed && read(root / "protected") == "keep", "existing file protected");
#ifdef _WIN32
        const auto outside = root / "outside-dir";
        fs::create_directory(outside);
        std::ofstream(outside / "keep") << "keep";
        junction(root / "junction", outside);
        check(run(root / "archive.zip", root / "junction").result == ExtractOutcome::Result::Failed, "existing reparse destination rejected");
        check(run(root / "archive.zip", root / "junction" / "child").result == ExtractOutcome::Result::Failed &&
            !fs::exists(outside / "child") && read(outside / "keep") == "keep", "reparse ancestor rejected");
        bool attempted = false;
        const auto guarded = native_extract(root / "archive.zip", root / "guarded", {}, [&](const std::string&) {
            attempted = true;
            check(!MoveFileW((root / "guarded").c_str(), (root / "moved").c_str()), "destination cannot be replaced while extracting");
        }, {});
        check(attempted && guarded.result == ExtractOutcome::Result::Done, "directory guards");
        const auto foreign = native_extract(root / "archive.zip", root / "foreign-output", [] { return true; },
            [&](const std::string&) { std::ofstream(root / "foreign-output" / "foreign.txt") << "keep"; }, {});
        check(foreign.result == ExtractOutcome::Result::Cancelled && foreign.error.find("cannot remove") != std::string::npos &&
            read(root / "foreign-output" / "foreign.txt") == "keep", "rollback reports unowned occupants without deleting them");
        const auto intercepted = native_extract(root / "archive.tar", root / "intercepted", {}, [&](const std::string&) {
            junction(root / "intercepted" / "folder", outside);
        }, {});
        check(intercepted.result == ExtractOutcome::Result::Done && intercepted.unsafe > 0 &&
            !fs::exists(outside / "data.bin"), "injected junction is never followed");
        check(RemoveDirectoryW((root / "intercepted" / "folder").c_str()) && RemoveDirectoryW((root / "junction").c_str()), "remove test junctions only");
#endif
        fs::remove_all(root);
        std::cout << "All native extraction tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
