// ArchFtchX.elf: ArchiveFetch's archive extractor. ArchiveFetch sends it to the console's own ELF loader for each
// archive, so decompression never runs inside ArchiveFetch (ps5upload found it can crash a long-running payload).
// Single-threaded. It reads and deletes the request, reports its process ID over a localhost connection that stays
// open until it exits, and writes its progress and result to the status file.
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(PS5_PAYLOAD)
#include <dlfcn.h>
#endif

#include <nlohmann/json.hpp>

#include "diag_log.hpp"
#include "extract_engine.hpp"
#include "extract_protocol.hpp"

using namespace archivefetch_extract;
using archivefetch_diag::field;
using archivefetch_diag::log_event;
using Json = nlohmann::json;

namespace {

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void write_status(const Json& status) {
    const std::string temporary = std::string(status_path) + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << status.dump();
    }
    rename(temporary.c_str(), status_path);
}

Json progress_json(Json status, const Progress& progress) {
    status["files"] = progress.files;
    status["bytes"] = progress.bytes;
    status["consumed"] = progress.consumed;
    status["unsafe"] = progress.unsafe;
    status["links"] = progress.links;
    status["current"] = progress.current.substr(0, 512);
    return status;
}

int report_pid(int port) {
    const int connection = socket(AF_INET, SOCK_STREAM, 0);
    if (connection < 0) return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    const std::int32_t pid = getpid();
    if (connect(connection, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        send(connection, &pid, sizeof(pid), 0) != static_cast<ssize_t>(sizeof(pid))) {
        close(connection);
        return -1;
    }
    return connection;
}

}

int main() {
    mkdir(data_root, 0777);
    mkdir(log_directory, 0777);
    mkdir(work_directory, 0777);
    archivefetch_diag::log_rotate(log_path, log_previous_path, 256 * 1024);
    int open_error = 0;
    archivefetch_diag::log_open(log_directory, log_path, &open_error);
#if defined(PS5_PAYLOAD)
    if (const auto setter = reinterpret_cast<int (*)(const char*)>(dlsym(RTLD_DEFAULT, "sceKernelSetProcessName"))) setter("ArchFtchX.elf");
#endif
    std::signal(SIGPIPE, SIG_IGN);
    archivefetch_diag::log_install_crash_handler();

    Json request;
    {
        std::ifstream input(request_path, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        request = Json::parse(text, nullptr, false);
    }
    unlink(request_path);
    if (!request.is_object() || !request.contains("archive") || !request.contains("destination") || !request.contains("report_port")) {
        log_event("EXTRACT_REQUEST_INVALID");
        archivefetch_diag::log_close();
        return 1;
    }
    Options options;
    options.archive = request.at("archive").get<std::string>();
    options.destination = request.at("destination").get<std::string>();
    const auto job = request.value("job", std::string{});
    const int report = report_pid(request.at("report_port").get<int>());
    struct stat archive_info {};
    const long long archive_size = stat(options.archive.c_str(), &archive_info) == 0 ? static_cast<long long>(archive_info.st_size) : 0;
    log_event("EXTRACT_REQUEST", {field("job", job.c_str()), field("archive", options.archive.c_str()),
        field("destination", options.destination.c_str()), field("bytes", archive_size), field("reported", report >= 0 ? "true" : "false")});

    const Json base = {{"job", job}, {"pid", static_cast<long long>(getpid())}, {"archive_size", archive_size}};
    Json status = base;
    status["state"] = "starting";
    write_status(status);

    Progress progress;
    long long last_status = 0, last_cancel_check = 0;
    bool cancel_seen = false;
    options.cancelled = [&] {
        const auto now = now_ms();
        if (now - last_cancel_check >= 150) {
            last_cancel_check = now;
            struct stat cancel {};
            cancel_seen = stat(cancel_path, &cancel) == 0;
        }
        return cancel_seen;
    };
    // "running" tells ArchiveFetch that the destination folder now exists and belongs to this run.
    options.started = [&] {
        auto running = base;
        running["state"] = "running";
        write_status(running);
        last_status = now_ms();
    };
    options.progress = [&](const Progress& current) {
#ifdef ARCHIVEFETCH_EXTRACT_TEST_DELAY_MS
        usleep(ARCHIVEFETCH_EXTRACT_TEST_DELAY_MS * 1000);
#endif
        const auto now = now_ms();
        if (now - last_status < 250) return;
        last_status = now;
        auto running = base;
        running["state"] = "running";
        write_status(progress_json(running, current));
    };

    const auto started = now_ms();
    std::string error;
    const Result result = extract_archive(options, progress, error);
    const char* state = result == Result::Done ? "done" : result == Result::Cancelled ? "cancelled" : "failed";
    auto final_status = progress_json(base, progress);
    final_status["state"] = state;
    final_status["error"] = error;
    write_status(final_status);
    log_event("EXTRACT_RESULT", {field("job", job.c_str()), field("state", state), field("error", error.c_str()),
        field("files", static_cast<long long>(progress.files)), field("bytes", static_cast<long long>(progress.bytes)),
        field("unsafe", static_cast<long long>(progress.unsafe)), field("links", static_cast<long long>(progress.links)),
        field("ms", now_ms() - started)});
    unlink(cancel_path);
    archivefetch_diag::log_close();
    if (report >= 0) close(report);
    return 0;
}
