#include "extract_runner.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

#include "diag_log.hpp"
#include "extract_protocol.hpp"
#include "extractor_elf.hpp"
#include "remove_tree.hpp"

namespace archivefetch_diag {
namespace {

using archive_catalog::ExtractOutcome;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr int start_timeout_ms = 20000;
// Without new files, bytes or archive progress for this long, the extractor is treated as hung.
constexpr int stall_timeout_ms = 120000;
constexpr int stop_grace_ms = 10000;

long long since(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

bool readable(int fd, int milliseconds) {
    pollfd entry{fd, POLLIN, 0};
    return poll(&entry, 1, milliseconds) > 0;
}

// Non-blocking connect with a timeout, so a missing loader cannot stall the download worker.
int connect_local(int port, int milliseconds, int* error) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { *error = errno; return -1; }
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (errno != EINPROGRESS) { *error = errno; close(fd); return -1; }
        pollfd entry{fd, POLLOUT, 0};
        int result = 0;
        socklen_t length = sizeof(result);
        if (poll(&entry, 1, milliseconds) <= 0) { *error = ETIMEDOUT; close(fd); return -1; }
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &length);
        if (result != 0) { *error = result; close(fd); return -1; }
    }
    fcntl(fd, F_SETFL, flags);
    return fd;
}

bool send_extractor(int* error) {
    const int loader = connect_local(archivefetch_extract::loader_port, 5000, error);
    if (loader < 0) return false;
    timeval timeout{10, 0};
    setsockopt(loader, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    size_t sent = 0;
    while (sent < sizeof(extractor_elf)) {
        const ssize_t written = send(loader, extractor_elf + sent, sizeof(extractor_elf) - sent, 0);
        if (written <= 0) { *error = errno; break; }
        sent += static_cast<size_t>(written);
    }
    shutdown(loader, SHUT_WR);
    // Give the loader a moment to read everything and close its side first.
    readable(loader, 2000);
    close(loader);
    return sent == sizeof(extractor_elf);
}

Json read_status() {
    std::ifstream input(archivefetch_extract::status_path, std::ios::binary);
    if (!input) return Json();
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return Json::parse(text, nullptr, false);
}

std::string size_text(unsigned long long bytes) {
    char text[32];
    if (bytes >= (1ull << 30)) std::snprintf(text, sizeof(text), "%.1f GB", bytes / 1073741824.0);
    else std::snprintf(text, sizeof(text), "%.0f MB", bytes / 1048576.0);
    return text;
}

}

bool send_to_local_loader(const std::filesystem::path& elf, std::string& error) {
    std::ifstream input(elf, std::ios::binary);
    if (!input) { error = "cannot read the payload"; return false; }
    int code = 0;
    const int loader = connect_local(archivefetch_extract::loader_port, 5000, &code);
    if (loader < 0) { error = std::strerror(code); return false; }
    timeval timeout{30, 0};
    setsockopt(loader, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    std::vector<char> buffer(65536);
    bool sent = true;
    while (sent && input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        for (std::streamsize offset = 0, size = input.gcount(); offset < size;) {
            const ssize_t written = send(loader, buffer.data() + offset, static_cast<size_t>(size - offset), 0);
            if (written <= 0) { error = std::strerror(errno); sent = false; break; }
            offset += written;
        }
    }
    shutdown(loader, SHUT_WR);
    // Give the loader a moment to read everything and close its side first.
    readable(loader, 2000);
    close(loader);
    return sent;
}

ExtractOutcome run_extraction(const std::filesystem::path& archive, const std::filesystem::path& folder,
    const std::function<bool()>& cancelled, const std::function<void(const std::string&)>& status,
    const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
    using Result = ExtractOutcome::Result;
    const auto started = Clock::now();
    ExtractOutcome outcome;
    const std::string job = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    log_event("EXTRACT_BEGIN", {field("job", job.c_str()), field("archive", archive.c_str()), field("folder", folder.c_str())});
    const auto finish = [&](Result result, std::string error) {
        outcome.result = result;
        outcome.error = std::move(error);
        log_event("EXTRACT_END", {field("job", job.c_str()),
            field("result", result == Result::Done ? "done" : result == Result::Cancelled ? "cancelled" : "failed"),
            field("error", outcome.error.c_str()), field("files", static_cast<long long>(outcome.files)),
            field("bytes", static_cast<long long>(outcome.bytes)), field("unsafe", static_cast<long long>(outcome.unsafe)),
            field("links", static_cast<long long>(outcome.links)), field("ms", since(started))});
        return outcome;
    };

    mkdir(archivefetch_extract::data_root, 0777);
    mkdir(archivefetch_extract::work_directory, 0777);
    unlink(archivefetch_extract::status_path);
    unlink(archivefetch_extract::cancel_path);
    if (cancelled()) return finish(Result::Cancelled, "stopped");

    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    socklen_t length = sizeof(address);
    if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 1) != 0 ||
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        const int error = errno;
        if (listener >= 0) close(listener);
        return finish(Result::Failed, std::string("cannot prepare the extractor: ") + std::strerror(error));
    }

    {
        const Json request = {{"job", job}, {"archive", archive.string()}, {"destination", folder.string()}, {"report_port", ntohs(address.sin_port)}};
        const std::string temporary = std::string(archivefetch_extract::request_path) + ".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << request.dump();
        output.close();
        if (!output || rename(temporary.c_str(), archivefetch_extract::request_path) != 0) {
            close(listener);
            return finish(Result::Failed, "cannot write the extraction request");
        }
    }

    status("Starting the extractor");
    int send_error = 0;
    if (!send_extractor(&send_error)) {
        close(listener);
        unlink(archivefetch_extract::request_path);
        return finish(Result::Failed, "the console's ELF loader did not accept the extractor on port " +
            std::to_string(archivefetch_extract::loader_port) + " (" + std::strerror(send_error) + ")");
    }
    if (!readable(listener, start_timeout_ms)) {
        close(listener);
        unlink(archivefetch_extract::request_path);
        return finish(Result::Failed, "the extractor did not start");
    }
    const int report = accept(listener, nullptr, nullptr);
    close(listener);
    std::int32_t pid = 0;
    if (report < 0 || !readable(report, 5000) || recv(report, &pid, sizeof(pid), MSG_WAITALL) != static_cast<ssize_t>(sizeof(pid)) || pid <= 0) {
        if (report >= 0) close(report);
        return finish(Result::Failed, "the extractor did not report in");
    }
    log_event("EXTRACT_STARTED", {field("job", job.c_str()), field("pid", static_cast<long long>(pid))});

    Json last;
    std::string last_progress;
    auto progress_time = Clock::now(), stop_time = Clock::now(), kill_time = Clock::now();
    bool stop_sent = false, killed = false, stalled = false;
    for (;;) {
        // The kernel closes the report connection when the extractor ends, however it ends.
        if (readable(report, 250)) {
            char byte = 0;
            if (recv(report, &byte, 1, 0) <= 0) break;
        }
        const auto current = read_status();
        if (current.is_object() && current.value("job", std::string{}) == job) last = current;
        if (last.is_object() && last.value("state", std::string{}) == "running") {
            const auto files = last.value("files", 0ull), bytes = last.value("bytes", 0ull), consumed = last.value("consumed", 0ull);
            const auto total = last.value("archive_size", 0ull);
            const auto key = std::to_string(files) + "/" + std::to_string(bytes) + "/" + std::to_string(consumed);
            if (key != last_progress) {
                last_progress = key;
                progress_time = Clock::now();
                if (progress && total) progress(std::min(consumed, total), total);
                if (!stop_sent) status(std::to_string(files) + " files, " + size_text(bytes) + " extracted");
            }
        }
        if (!stop_sent && cancelled()) {
            stop_sent = true;
            stop_time = Clock::now();
            std::ofstream(archivefetch_extract::cancel_path).put('\n');
            status("Stopping the extraction");
        }
        if (!killed && ((stop_sent && since(stop_time) > stop_grace_ms) || (!stop_sent && since(progress_time) > stall_timeout_ms))) {
            stalled = !stop_sent;
            killed = true;
            kill_time = Clock::now();
            const int result = kill(pid, SIGKILL);
            log_event("EXTRACT_KILLED", {field("job", job.c_str()), field("pid", static_cast<long long>(pid)),
                field("reason", stalled ? "stalled" : "stop"), field("result", result)});
        }
        if (killed && since(kill_time) > 10000) break;
    }
    close(report);

    const auto final_status = read_status();
    if (final_status.is_object() && final_status.value("job", std::string{}) == job) last = final_status;
    const auto state = last.is_object() ? last.value("state", std::string{}) : std::string{};
    if (last.is_object()) {
        outcome.files = last.value("files", 0ull);
        outcome.bytes = last.value("bytes", 0ull);
        outcome.unsafe = last.value("unsafe", 0ull);
        outcome.links = last.value("links", 0ull);
    }
    unlink(archivefetch_extract::cancel_path);
    if (state == "done") return finish(Result::Done, {});
    if (state == "cancelled") return finish(Result::Cancelled, "stopped");
    if (state == "failed") return finish(Result::Failed, last.value("error", std::string("extraction failed")));
    // The extractor ended without a result; a folder it had started belongs to this run and is removed.
    if (state == "running") {
        std::error_code ignored;
        archive_catalog::remove_tree(folder, ignored);
    }
    if (stop_sent) return finish(Result::Cancelled, "stopped");
    return finish(Result::Failed, stalled ? "the extractor stopped making progress" : "the extractor stopped unexpectedly");
}

}
