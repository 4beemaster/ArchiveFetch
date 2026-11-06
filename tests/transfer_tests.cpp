#include "transfer.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace archive_catalog;

template <typename Action>
void rejects(Action action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    if (!rejected) throw std::runtime_error("Expected rejection");
}

int main() {
    auto directory = std::filesystem::temp_directory_path() /
        ("archive-catalog-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        for (long status : {401L, 403L}) {
            const auto detail = "HTTP " + std::to_string(status);
            if (request_error_message(detail, status, false) != detail + "; Archive.org denied access: S3 keys may be required. Add them in Settings." ||
                request_error_message(detail, status, true) != detail + "; Archive.org denied access: S3 keys were provided. Check them in Settings; this account may lack permission.")
                throw std::runtime_error("Access-denied guidance must distinguish missing and supplied keys");
        }
        for (bool provided : {false, true}) {
            if (request_error_message("Server unavailable", 503, provided) != "Server unavailable")
                throw std::runtime_error("Other errors must not suggest changing S3 keys");
        }
        if (S3Credentials{"access", "secret"}.authorization() != "Authorization: LOW access:secret" ||
            !S3Credentials{}.authorization().empty()) throw std::runtime_error("Incorrect S3 authorization");
        rejects([] { S3Credentials{"access", ""}.authorization(); });
        rejects([] { S3Credentials{"", "secret"}.authorization(); });
        rejects([] { S3Credentials{"access", "secret\r\nInjected: true"}.authorization(); });
        rejects([] { S3Credentials{"access:other", "secret"}.authorization(); });
        for (const auto& url : {"https://archive.org/download/item/file.chd", "https://ia800000.us.archive.org/file.chd"}) {
            if (!s3_destination_allowed(url)) throw std::runtime_error("Archive.org S3 destination rejected");
        }
        for (const auto& url : {"http://archive.org/file", "https://archive.org.evil.test/file", "https://evilarchive.org/file",
                                "https://archive.org:444/file", "https://user:pass@archive.org/file", "https://example.com/file"}) {
            if (s3_destination_allowed(url)) throw std::runtime_error("Unsafe S3 destination allowed");
        }
        File file{"example", "Folder/test.chd", 3, "900150983cd24fb0d6963f7d28e17f72", "a9993e364706816aba3e25717850c26c9cd0d89d"};
        auto target = download_target(directory, file);
        if (target != std::filesystem::canonical(directory) / "test.chd" ||
            std::filesystem::exists(directory / "example") || std::filesystem::exists(directory / "Folder")) {
            throw std::runtime_error("Downloads must be placed directly in the selected directory");
        }
        { std::ofstream output(target, std::ios::binary); output << "abc"; }
        { Http http; http.download(file, directory); }
        File collision{"other-item", "Other/test.chd", 4, {}, "a9993e364706816aba3e25717850c26c9cd0d89d"};
        rejects([&] { Http http; http.download(collision, directory); });
        verify_file(file, target);
        auto replacement = target.string() + ".part";
        { std::ofstream output(target, std::ios::binary); output << "old"; }
        { std::ofstream output(replacement, std::ios::binary); output << "bad"; }
        rejects([&] { Http http; http.download(file, directory, true); });
        {
            std::ifstream input(target);
            std::string original;
            input >> original;
            if (original != "old" || std::filesystem::exists(replacement)) {
                throw std::runtime_error("Invalid replacement must not modify the existing file");
            }
        }
        { std::ofstream output(replacement, std::ios::binary); output << "abc"; }
        {
            TransferHooks cancelled;
            cancelled.cancelled = [] { return true; };
            rejects([&] { Http http({}, cancelled); http.download(file, directory, true); });
            std::ifstream input(target);
            std::string original;
            input >> original;
            if (original != "old" || !std::filesystem::exists(replacement)) {
                throw std::runtime_error("Cancelled overwrite must preserve original and resumable data");
            }
        }
        { Http http; http.download(file, directory, true); }
        verify_file(file, target);
        if (std::filesystem::exists(replacement)) throw std::runtime_error("Verified replacement was not published");
        file.sha1.clear();
        verify_file(file, target);
        file.md5 = "00000000000000000000000000000000";
        rejects([&] { verify_file(file, target); });
        file.size = 4;
        rejects([&] { verify_file(file, target); });
        file.name = "../escape.chd";
        rejects([&] { download_target(directory, file); });
        File interrupted{"example", "cancelled.chd", 3, {}, "a9993e364706816aba3e25717850c26c9cd0d89d"};
        auto interrupted_target = download_target(directory, interrupted);
        auto partial = interrupted_target.string() + ".part";
        { std::ofstream output(partial, std::ios::binary); output << "abc"; }
        int checks = 0;
        TransferHooks hooks;
        hooks.cancelled = [&] { return ++checks >= 2; };
        {
            Http http({}, hooks);
            rejects([&] { http.download(interrupted, directory); });
        }
        if (!std::filesystem::exists(partial) || std::filesystem::exists(interrupted_target)) {
            throw std::runtime_error("Cancellation must preserve resumable data without publishing a final file");
        }
        file.name = "blocked/test.chd";
        std::filesystem::remove(target);
        std::filesystem::create_directory(target);
        rejects([&] { download_target(directory, file); });
        std::filesystem::remove(target);
        std::filesystem::create_directory(target.string() + ".part");
        rejects([&] { download_target(directory, file); });
        std::filesystem::remove_all(directory);
        std::cout << "All transfer integrity tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(directory);
        std::cerr << error.what() << '\n';
        return 1;
    }
}