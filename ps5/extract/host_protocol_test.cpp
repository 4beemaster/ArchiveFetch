// Native test of ArchiveFetch's side of extraction (extract_runner.cpp) against the real extractor program,
// started by a fake ELF loader in this process. Run through host_test.sh; nothing here runs on the console.
#include <archive.h>
#include <archive_entry.h>

#include <arpa/inet.h>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "extract_protocol.hpp"
#include "extract_runner.hpp"

namespace fs = std::filesystem;
using archive_catalog::ExtractOutcome;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void make_zip(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& files) {
    archive* writer = archive_write_new();
    archive_write_set_format_zip(writer);
    archive_write_open_filename(writer, path.c_str());
    for (const auto& [name, content] : files) {
        archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, name.c_str());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(entry, static_cast<la_int64_t>(content.size()));
        archive_write_header(writer, entry);
        archive_write_data(writer, content.data(), content.size());
        archive_entry_free(entry);
    }
    archive_write_close(writer);
    archive_write_free(writer);
}

// Accepts payloads on the loader port and runs each one, like the console's ELF loader.
std::atomic<int> launched{0};
void fake_loader(int listener, fs::path folder) {
    for (;;) {
        const int connection = accept(listener, nullptr, nullptr);
        if (connection < 0) return;
        std::string payload;
        char buffer[65536];
        for (ssize_t count; (count = recv(connection, buffer, sizeof(buffer), 0)) > 0;) payload.append(buffer, static_cast<size_t>(count));
        close(connection);
        const auto program = folder / ("payload-" + std::to_string(++launched));
        std::ofstream(program, std::ios::binary) << payload;
        fs::permissions(program, fs::perms::owner_all);
        if (fork() == 0) {
            execl(program.c_str(), program.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
    }
}

ExtractOutcome extract(const fs::path& archive, const fs::path& folder, std::function<bool()> stop, std::vector<std::string>* updates = nullptr) {
    return archivefetch_diag::run_extraction(archive, folder, stop,
        [updates](const std::string& text) { if (updates) updates->push_back(text); }, {});
}

ExtractOutcome extract(const fs::path& archive, const fs::path& folder, std::vector<std::string>* updates = nullptr) {
    return extract(archive, folder, [] { return false; }, updates);
}

}

int main() {
    signal(SIGCHLD, SIG_IGN);
    const fs::path root = fs::path(archivefetch_extract::data_root).parent_path();
    const fs::path work = root / "work";
    fs::create_directories(work);
    try {
        make_zip(work / "pack.zip", {{"Pack/a.txt", "alpha"}, {"Pack/b/c.txt", "charlie"}});
        std::vector<std::pair<std::string, std::string>> many;
        for (int index = 0; index < 40; ++index) many.emplace_back("Many/file" + std::to_string(index) + ".txt", std::string(1000, 'x'));
        make_zip(work / "many.zip", many);

        auto outcome = extract(work / "pack.zip", work / "pack");
        require(outcome.result == ExtractOutcome::Result::Failed && outcome.error.find("loader") != std::string::npos,
            "without a loader the extraction fails cleanly: " + outcome.error);
        std::cout << "ok: no loader -> " << outcome.error << "\n";

        const int listener = socket(AF_INET, SOCK_STREAM, 0);
        int on = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(archivefetch_extract::loader_port);
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 4) == 0, "fake loader listens");
        std::thread(fake_loader, listener, work).detach();

        std::vector<std::string> updates;
        outcome = extract(work / "pack.zip", work / "pack", &updates);
        require(outcome.result == ExtractOutcome::Result::Done, "the extractor unpacks through the loader: " + outcome.error);
        require(read(work / "pack/Pack/a.txt") == "alpha" && read(work / "pack/Pack/b/c.txt") == "charlie" && outcome.files == 2, "files arrive");
        require(!updates.empty(), "progress is reported");
        require(!fs::exists(archivefetch_extract::request_path), "the request is consumed");
        std::cout << "ok: extract (" << updates.size() << " status updates, last: " << updates.back() << ")\n";

        outcome = extract(work / "pack.zip", work / "pack");
        require(outcome.result == ExtractOutcome::Result::Failed && outcome.error.find("already exists") != std::string::npos &&
            read(work / "pack/Pack/a.txt") == "alpha", "an existing folder is refused and kept: " + outcome.error);
        std::cout << "ok: existing folder -> " << outcome.error << "\n";

        outcome = extract(work / "pack.zip", work / "skipped", [] { return true; });
        require(outcome.result == ExtractOutcome::Result::Cancelled && !fs::exists(work / "skipped"), "a stop before starting starts nothing");

        // The native extractor waits per file (ARCHIVEFETCH_EXTRACT_TEST_DELAY_MS), so the stop lands mid-way.
        std::vector<std::string> stopping;
        outcome = extract(work / "many.zip", work / "stopped", [&stopping] {
            for (const auto& text : stopping) if (text.find("extracted") != std::string::npos) return true;
            return false;
        }, &stopping);
        require(outcome.result == ExtractOutcome::Result::Cancelled && !fs::exists(work / "stopped"), "a stop mid-way removes the folder: " + outcome.error);
        require(!fs::exists(archivefetch_extract::cancel_path), "the stop request is cleaned up");
        std::cout << "ok: stop\n";

        std::ofstream(work / "junk.zip") << "not an archive";
        outcome = extract(work / "junk.zip", work / "junk");
        require(outcome.result == ExtractOutcome::Result::Failed && !fs::exists(work / "junk"), "a bad archive fails: " + outcome.error);
        std::cout << "ok: bad archive -> " << outcome.error << "\n";

        require(fs::exists(archivefetch_extract::log_path), "the extractor writes its own log");
        std::cout << "All extraction protocol tests passed (" << launched << " extractor runs)\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "FAILED: " << failure.what() << '\n';
        return 1;
    }
}
