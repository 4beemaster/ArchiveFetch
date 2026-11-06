// Native tests for the extractor engine: builds archives with libarchive's writer and checks what is written.
// Run through host_test.sh; nothing here runs on the console.
#include <archive.h>
#include <archive_entry.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unistd.h>
#include <vector>

#include "extract_engine.hpp"
#include "remove_tree.hpp"

namespace fs = std::filesystem;
using namespace archivefetch_extract;

namespace {

struct Entry {
    std::string name;
    std::string content;
    int type = AE_IFREG;
    std::string link;
    bool hardlink = false;
};

int checks = 0;

void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string pattern(std::size_t size) {
    std::string text(size, '\0');
    unsigned value = 12345;
    for (auto& character : text) { value = value * 1103515245 + 12345; character = static_cast<char>(value >> 16); }
    return text;
}

// Returns false when this libarchive build cannot write the format or option.
bool make_archive(const fs::path& path, const char* format, const char* filter, const std::vector<Entry>& entries, const char* option = nullptr) {
    archive* writer = archive_write_new();
    if (archive_write_set_format_by_name(writer, format) != ARCHIVE_OK) { archive_write_free(writer); return false; }
    if (filter && archive_write_add_filter_by_name(writer, filter) != ARCHIVE_OK) { archive_write_free(writer); return false; }
    if (option && archive_write_set_options(writer, option) != ARCHIVE_OK) { archive_write_free(writer); return false; }
    if (archive_write_open_filename(writer, path.c_str()) != ARCHIVE_OK) throw std::runtime_error(archive_error_string(writer));
    for (const auto& item : entries) {
        archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, item.name.c_str());
        archive_entry_set_mtime(entry, 1700000000, 0);
        if (item.hardlink) {
            archive_entry_set_hardlink(entry, item.link.c_str());
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_size(entry, 0);
        } else if (item.type == AE_IFLNK) {
            archive_entry_set_filetype(entry, AE_IFLNK);
            archive_entry_set_symlink(entry, item.link.c_str());
            archive_entry_set_perm(entry, 0777);
        } else {
            archive_entry_set_filetype(entry, item.type);
            archive_entry_set_perm(entry, item.type == AE_IFDIR ? 0755 : 0644);
            archive_entry_set_size(entry, item.type == AE_IFREG ? static_cast<la_int64_t>(item.content.size()) : 0);
        }
        if (archive_write_header(writer, entry) != ARCHIVE_OK) {
            const std::string message = archive_error_string(writer) ? archive_error_string(writer) : "header";
            archive_entry_free(entry);
            archive_write_free(writer);
            throw std::runtime_error(std::string(format) + " header for " + item.name + ": " + message);
        }
        if (!item.content.empty() && item.type == AE_IFREG && !item.hardlink)
            archive_write_data(writer, item.content.data(), item.content.size());
        archive_entry_free(entry);
    }
    archive_write_close(writer);
    archive_write_free(writer);
    return true;
}

Result run(const fs::path& archive, const fs::path& destination, Progress& progress, std::string& error, std::function<bool()> cancelled = {}) {
    Options options;
    options.archive = archive.string();
    options.destination = destination.string();
    options.cancelled = std::move(cancelled);
    return extract_archive(options, progress, error);
}

}

int main() {
    const fs::path root = fs::temp_directory_path() / ("archivefetch-extract-test-" + std::to_string(getpid()));
    fs::create_directories(root);
    try {
        require(safe_entry_path("a/b.txt") == "a/b.txt", "plain names stay");
        require(safe_entry_path("./a//b/") == "a/b", "dot and empty components are dropped");
        require(safe_entry_path("../x").empty() && safe_entry_path("a/../../x").empty() && safe_entry_path("/x").empty() &&
            safe_entry_path("C:/x").empty() && safe_entry_path("..\\x").empty(), "traversal, absolute and drive names are refused");
        require(safe_entry_path("bad:name?.txt") == "bad_name_.txt" && safe_entry_path("dots./x") == "dots_/x" &&
            safe_entry_path(std::string("ctl\x01.txt")) == "ctl_.txt", "unsafe characters become _");
        require(safe_entry_path(std::string(256, 'x')).empty(), "over-long names are refused");
        {
            const auto tree = root / "Tree", outside = root / "outside.txt";
            fs::create_directories(tree / "a" / "b" / "c");
            fs::create_directories(tree / "empty");
            std::ofstream(tree / "a" / "b" / "c" / "deep.bin") << "x";
            std::ofstream(tree / "top.txt") << "x";
            std::ofstream(outside) << "keep";
            fs::create_symlink(outside, tree / "a" / "link.txt");
            fs::create_directory_symlink(root, tree / "a" / "loop");
            std::error_code failure;
            require(archive_catalog::tree_size(tree) == 2, "tree_size counts only real files: " + std::to_string(archive_catalog::tree_size(tree)));
            std::uintmax_t freed = 0;
            require(archive_catalog::remove_tree(tree, failure, &freed) && !failure && !fs::exists(fs::symlink_status(tree)) && freed == 2, "remove_tree deletes a nested folder and counts its bytes");
            require(read(outside) == "keep" && fs::exists(root), "remove_tree removes links without following them");
            require(archive_catalog::remove_tree(tree, failure) && !failure, "remove_tree accepts a folder that is already gone");
            fs::remove(outside);
        }

        const auto big = pattern(3 * 1024 * 1024 + 17);
        const std::vector<Entry> normal = {{"Game/", "", AE_IFDIR}, {"Game/readme.txt", "hello"}, {"Game/data/big.bin", big},
            {"empty.txt", ""}, {"Game/emptydir/", "", AE_IFDIR}};
        for (const auto& [format, filter, suffix] : std::vector<std::tuple<const char*, const char*, const char*>>{
                 {"zip", nullptr, "zip"}, {"7zip", nullptr, "7z"}, {"pax", "gzip", "tar.gz"}, {"pax", "xz", "tar.xz"}, {"ustar", nullptr, "tar"}}) {
            const auto archive = root / (std::string("normal.") + suffix);
            if (!make_archive(archive, format, filter, normal)) { std::cout << "skip: cannot write " << suffix << "\n"; continue; }
            const auto destination = root / (std::string("normal-") + suffix);
            Progress progress;
            std::string error;
            const auto result = run(archive, destination, progress, error);
            require(result == Result::Done, std::string(suffix) + " extracts: " + error);
            require(read(destination / "Game/readme.txt") == "hello" && read(destination / "Game/data/big.bin") == big, std::string(suffix) + " contents match");
            require(fs::is_regular_file(destination / "empty.txt") && fs::file_size(destination / "empty.txt") == 0, std::string(suffix) + " keeps empty files");
            require(fs::is_directory(destination / "Game/emptydir"), std::string(suffix) + " keeps empty folders");
            require(progress.files == 3 && progress.bytes == big.size() + 5, std::string(suffix) + " counts files and bytes");
            require(fs::last_write_time(destination / "Game/readme.txt").time_since_epoch().count() != 0, std::string(suffix) + " sets times");
            for (const auto& item : fs::recursive_directory_iterator(destination))
                require(item.path().filename().string().rfind(".archivefetch-x-", 0) != 0, std::string(suffix) + " leaves no temporary files");
            std::cout << "ok: " << suffix << "\n";
        }

        const auto hostile = root / "hostile.tar.gz";
        make_archive(hostile, "pax", "gzip", {{"../evil.txt", "x"}, {"/abs.txt", "x"}, {"a/../../b.txt", "x"}, {"C:/drive.txt", "x"},
            {"link", "", AE_IFLNK, "/etc/passwd"}, {"good.txt", "good"}, {"hard", "", AE_IFREG, "good.txt", true},
            {"bad:name?.txt", "colon"}, {"dots./x.txt", "dots"}, {"x", "file"}, {"x/y.txt", "under a file"}, {"dup.txt", "first"}, {"dup.txt", "second"}});
        {
            Progress progress;
            std::string error;
            const auto destination = root / "hostile";
            require(run(hostile, destination, progress, error) == Result::Done, "hostile archive still extracts its safe files: " + error);
            require(!fs::exists(root / "evil.txt") && !fs::exists(root / "b.txt") && !fs::exists("/abs.txt"), "nothing is written outside the folder");
            require(!fs::exists(destination / "link") && !fs::exists(destination / "hard"), "links are never created");
            require(read(destination / "bad_name_.txt") == "colon" && read(destination / "dots_/x.txt") == "dots", "unsafe characters are replaced");
            require(read(destination / "x") == "file" && !fs::exists(destination / "x/y.txt"), "a file in the way refuses entries below it");
            require(read(destination / "dup.txt") == "second", "a repeated name keeps the last copy");
            require(progress.unsafe == 5 && progress.links == 2, "unsafe and link counts: " + std::to_string(progress.unsafe) + "/" + std::to_string(progress.links));
            std::cout << "ok: hostile names\n";
        }
        {
            const auto existing = root / "existing";
            fs::create_directories(existing);
            std::ofstream(existing / "keep.txt") << "keep";
            Progress progress;
            std::string error;
            require(run(root / "normal.zip", existing, progress, error) == Result::Failed && error.find("already exists") != std::string::npos,
                "an existing folder is refused");
            require(read(existing / "keep.txt") == "keep" && std::distance(fs::directory_iterator(existing), fs::directory_iterator()) == 1,
                "an existing folder is left untouched");
            std::cout << "ok: existing folder\n";
        }
        {
            Progress progress;
            std::string error;
            const auto destination = root / "cancelled";
            int calls = 0;
            require(run(root / "normal.tar.gz", destination, progress, error, [&] { return ++calls > 3; }) == Result::Cancelled, "a stop is reported");
            require(!fs::exists(destination), "a stopped extraction removes its folder");
            std::cout << "ok: stop\n";
        }
        {
            const auto data = read(root / "normal.zip");
            const auto broken = root / "broken.zip";
            std::ofstream(broken, std::ios::binary) << data.substr(0, data.size() / 2);
            Progress progress;
            std::string error;
            require(run(broken, root / "broken", progress, error) == Result::Failed && !fs::exists(root / "broken"), "a damaged archive fails and leaves nothing: " + error);
            std::ofstream(root / "text.zip") << "this is not an archive";
            require(run(root / "text.zip", root / "text", progress, error) == Result::Failed && !fs::exists(root / "text"), "a non-archive fails and leaves nothing");
            require(run(root / "missing.zip", root / "missing", progress, error) == Result::Failed && !fs::exists(root / "missing"), "a missing archive fails");
            std::cout << "ok: damaged, non-archive and missing\n";
        }
        if (make_archive(root / "locked.zip", "zip", nullptr, {{"secret.txt", "secret"}}, "zip:encryption=zipcrypt,zip:passphrase=pw")) {
            Progress progress;
            std::string error;
            require(run(root / "locked.zip", root / "locked", progress, error) == Result::Failed && error.find("password") != std::string::npos &&
                !fs::exists(root / "locked"), "password-protected archives are refused: " + error);
            std::cout << "ok: password-protected zip\n";
        } else {
            std::cout << "skip: cannot write an encrypted zip\n";
        }
        fs::remove_all(root);
        std::cout << "All extractor tests passed (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "FAILED: " << failure.what() << '\n';
        return 1;
    }
}
