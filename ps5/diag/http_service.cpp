#include "http_service.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

#include "diag_httplib.hpp"

#include "api_routes.hpp"
#include "diag_config.hpp"
#include "diag_identity.hpp"
#include "diag_log.hpp"
#include "web_assets.hpp"

namespace archivefetch_diag {
namespace {

constexpr int worker_count = 4;
constexpr long long repeat_log_interval_ms = 60000;
constexpr size_t repeat_table_limit = 256;
constexpr int relisten_interval_ms = 5000;

std::unique_ptr<httplib::Server> server;
std::mutex server_mutex;
std::atomic<bool> closing{false};
std::thread listener;
std::mutex shutdown_mutex;
std::condition_variable shutdown_signal;
bool shutdown_requested = false;
std::atomic<long long> request_count{0};
timespec started{};

long long monotonic_ms() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

long long uptime_seconds() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec - started.tv_sec;
}

void request_shutdown() {
    {
        std::lock_guard<std::mutex> lock(shutdown_mutex);
        shutdown_requested = true;
    }
    shutdown_signal.notify_all();
}

// Every worker names itself, so each thread the loader sees is ArchFtch.elf.
class NamedWorkerPool final : public httplib::TaskQueue {
public:
    explicit NamedWorkerPool(int count) {
        try {
            for (int index = 0; index < count; ++index) workers_.emplace_back([this, index] { run(index); });
        } catch (...) {
            shutdown();
            throw;
        }
    }

    bool enqueue(std::function<void()> job) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return false;
            jobs_.push_back(std::move(job));
        }
        ready_.notify_one();
        return true;
    }

    void shutdown() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        for (std::thread& worker : workers_)
            if (worker.joinable()) worker.join();
    }

private:
    void run(int index) {
        log_event("THREAD_NAME_END", {field("role", "http-worker"), field("worker", index),
            field("requested", process_name), field_hex("result", name_thread())});
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                if (jobs_.empty()) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
    }

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> jobs_;
    std::mutex mutex_;
    std::condition_variable ready_;
    bool stopping_ = false;
};

struct RepeatState {
    long long last_logged_ms = 0;
    long long suppressed = 0;
};
std::mutex repeat_mutex;
std::map<std::string, RepeatState> repeats;

// Identical requests (such as the UI polling /api/state) are logged at most once per minute with a count.
void log_request(const httplib::Request& request, const httplib::Response& response) {
    const std::string key = request.method + ' ' + request.path + ' ' + std::to_string(response.status);
    const long long now = monotonic_ms();
    long long suppressed = 0;
    {
        std::lock_guard<std::mutex> lock(repeat_mutex);
        if (repeats.size() >= repeat_table_limit && repeats.find(key) == repeats.end()) repeats.clear();
        RepeatState& state = repeats[key];
        if (state.last_logged_ms != 0 && now - state.last_logged_ms < repeat_log_interval_ms) {
            ++state.suppressed;
            return;
        }
        suppressed = state.suppressed;
        state.suppressed = 0;
        state.last_logged_ms = now;
    }
    log_event("HTTP_REQUEST", {field("method", request.method.c_str()), field("path", request.path.c_str()),
        field("status", response.status), field("bytes", static_cast<long long>(response.body.size())),
        field("peer", request.remote_addr.c_str()), field("repeats_since_last", suppressed)});
}

std::string content_type(const char* type) {
    const bool text = std::strncmp(type, "text/", 5) == 0 || std::strcmp(type, "application/javascript") == 0 ||
        std::strcmp(type, "application/json") == 0;
    return text ? std::string(type) + "; charset=utf-8" : std::string(type);
}

void configure_routes(httplib::Server& http) {
    http.set_post_routing_handler([](const httplib::Request&, httplib::Response& response) {
        response.set_header("Cache-Control", "no-store");
        response.set_header("X-Content-Type-Options", "nosniff");
    });
    http.set_logger(log_request);
    api_register_routes(http);

    http.Get("/status", [](const httplib::Request&, httplib::Response& response) {
        char body[256];
        std::snprintf(body, sizeof(body),
            "{\"service\":\"archivefetch\",\"version\":\"%s\",\"pid\":%d,\"uptime_s\":%lld,\"requests\":%lld}\n",
            version, static_cast<int>(getpid()), uptime_seconds(), request_count.load());
        response.set_content(body, content_type("application/json"));
    });
    http.Get("/diag", [](const httplib::Request&, httplib::Response& response) {
        char body[1024];
        std::snprintf(body, sizeof(body),
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
            "<title>ArchiveFetch status</title></head>"
            "<body style=\"font-family:sans-serif;background:#111;color:#eee;padding:2em\">"
            "<h1>ArchiveFetch</h1>"
            "<p>Version %s</p><p>Uptime %lld s, requests %lld</p><p>Log %s</p>"
            "<form method=\"post\" action=\"/shutdown\"><button>Stop diagnostic</button></form>"
            "</body></html>",
            version, uptime_seconds(), request_count.load(), log_path);
        response.set_content(body, content_type("text/html"));
    });
    http.Post("/shutdown", [](const httplib::Request& request, httplib::Response& response) {
        if (request.remote_addr != "127.0.0.1") {
            response.status = 403;
            response.set_content("shutdown is only accepted from the console\n", content_type("text/plain"));
            return;
        }
        // Browsers cannot send this header to another origin without a preflight, which is never answered.
        if (request.get_header_value("X-ArchiveFetch-Handoff") == "keys" && !request.has_header("Origin")) {
            std::string body = api_handoff_keys();
            response.set_content(body, content_type("application/json"));
            OPENSSL_cleanse(&body[0], body.size());
        } else {
            response.set_content("ArchiveFetch stopping\n", content_type("text/plain"));
        }
        request_shutdown();
    });
    http.Get(R"(/.*)", [](const httplib::Request& request, httplib::Response& response) {
        WebAsset asset{};
        if (!find_web_asset(request.path.c_str(), &asset)) {
            response.status = 404;
            response.set_content("not found\n", content_type("text/plain"));
            return;
        }
        response.set_content(reinterpret_cast<const char*>(asset.data), asset.size, content_type(asset.type));
    });
    http.set_pre_routing_handler([](const httplib::Request& request, httplib::Response& response) {
        ++request_count;
        return api_accept_peer(request, response) ? httplib::Server::HandlerResponse::Unhandled : httplib::Server::HandlerResponse::Handled;
    });
}

void set_timeouts(int fd, int seconds) {
    timeval timeout{};
    timeout.tv_sec = seconds;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

bool send_all(int fd, const char* data, size_t size) {
    while (size > 0) {
        const ssize_t sent = send(fd, data, size, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

sockaddr_in ipv4_address(int port, in_addr_t host) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(host);
    return address;
}

std::unique_ptr<httplib::Server> make_server() {
    auto http = std::make_unique<httplib::Server>();
    http->set_address_family(AF_INET);
    // httplib's default adds SO_REUSEPORT, which would let two copies share the port and defeat the handoff.
    http->set_socket_options([](socket_t sock) {
        const int enable = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    });
    http->new_task_queue = [] { return new NamedWorkerPool(worker_count); };
    http->set_read_timeout(3, 0);
    http->set_write_timeout(10, 0);
    http->set_keep_alive_timeout(2);
    http->set_payload_max_length(update_body_limit);
    configure_routes(*http);
    return http;
}

bool sleep_unless_closing(int milliseconds) {
    for (int slept = 0; slept < milliseconds; slept += 250) {
        if (closing.load()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return !closing.load();
}

bool replaced_by_another_copy(int port) {
    const LoopbackResponse status = http_loopback_request(port, "GET", "/status");
    char own_pid[32];
    std::snprintf(own_pid, sizeof(own_pid), "\"pid\":%d,", static_cast<int>(getpid()));
    // Matches both "archivefetch" and the diag builds' "archivefetch-diag".
    return status.status == 200 && std::strstr(status.body, "\"service\":\"archivefetch") != nullptr &&
        std::strstr(status.body, own_pid) == nullptr;
}

// Rest mode drops the network and ends httplib's accept loop; listen again once the port can be bound.
bool relisten(int port) {
    for (long long waited_ms = relisten_interval_ms;; waited_ms += relisten_interval_ms) {
        if (!sleep_unless_closing(relisten_interval_ms)) return false;
        if (http_port_free(port)) {
            std::unique_ptr<httplib::Server> fresh = make_server();
            if (fresh->bind_to_port("0.0.0.0", port)) {
                std::lock_guard<std::mutex> lock(server_mutex);
                if (closing.load()) return false;
                server = std::move(fresh);
                log_event("HTTP_LISTENER_RESTORED", {field("port", port), field("waited_ms", waited_ms)});
                return true;
            }
        } else if (replaced_by_another_copy(port)) {
            log_event("HTTP_REPLACED_WHILE_DOWN", {field("port", port), field("waited_ms", waited_ms)});
            request_shutdown();
            return false;
        }
        if (waited_ms % 60000 == 0) log_event("HTTP_RELISTEN_WAIT", {field("port", port), field("waited_ms", waited_ms)});
    }
}

void listen_loop(int port) {
    log_event("THREAD_NAME_END", {field("role", "http-listener"), field("requested", process_name),
        field_hex("result", name_thread())});
    for (;;) {
        httplib::Server* current = nullptr;
        {
            std::lock_guard<std::mutex> lock(server_mutex);
            current = server.get();
        }
        bool clean = false;
        try {
            clean = current->listen_after_bind();
        } catch (const std::exception& failure) {
            log_event("HTTP_LISTENER_FAILED", {field("error", failure.what())});
        }
        if (closing.load()) {
            log_event("HTTP_LISTENER_END", {field("clean", clean ? "true" : "false"),
                field("requests", request_count.load())});
            return;
        }
        log_event("HTTP_LISTENER_LOST", {field("clean", clean ? "true" : "false"),
            field("requests", request_count.load()), field("retry_ms", relisten_interval_ms)});
        if (!relisten(port)) {
            log_event("HTTP_LISTENER_END", {field("clean", "false"), field("requests", request_count.load())});
            return;
        }
    }
}

}

HttpStart http_start(int port) {
    clock_gettime(CLOCK_MONOTONIC, &started);
    {
        std::lock_guard<std::mutex> lock(shutdown_mutex);
        shutdown_requested = false;
    }
    closing.store(false);
    log_event("HTTP_START_BEGIN", {field("bind", "0.0.0.0"), field("port", port), field("workers", worker_count)});

    // httplib cannot report errno from bind, so check the port first to tell "in use" from other failures.
    if (!http_port_free(port)) {
        log_event("HTTP_BIND_FAILED", {field("port", port), field("in_use", "true")});
        return HttpStart::PortInUse;
    }

    server = make_server();
    if (!server->bind_to_port("0.0.0.0", port)) {
        const bool in_use = !http_port_free(port);
        log_event("HTTP_BIND_FAILED", {field("port", port), field("in_use", in_use ? "true" : "false")});
        server.reset();
        return in_use ? HttpStart::PortInUse : HttpStart::Failed;
    }

    try {
        listener = std::thread(listen_loop, port);
    } catch (const std::system_error& failure) {
        log_event("HTTP_THREAD_FAILED", {field("error", failure.what())});
        server.reset();
        return HttpStart::Failed;
    }
    server->wait_until_ready();
    log_event("HTTP_LISTENING", {field("port", port)});
    return HttpStart::Listening;
}

LoopbackResponse http_loopback_request(int port, const char* method, const char* path, const char* extra_headers) {
    LoopbackResponse response{};
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        response.error = errno;
        return response;
    }
    set_timeouts(fd, 5);

    const sockaddr_in address = ipv4_address(port, INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        response.error = errno;
        close(fd);
        return response;
    }
    response.connected = true;

    char request[512];
    const int request_size = std::snprintf(request, sizeof(request),
        "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\n%sConnection: close\r\nContent-Length: 0\r\n\r\n", method, path, extra_headers);
    char raw[4096] = "";
    size_t used = 0;
    if (request_size <= 0 || !send_all(fd, request, static_cast<size_t>(request_size))) {
        response.error = errno;
    } else {
        while (used < sizeof(raw) - 1) {
            const ssize_t got = recv(fd, raw + used, sizeof(raw) - 1 - used, 0);
            if (got < 0 && errno == EINTR) continue;
            if (got < 0) response.error = errno;
            if (got <= 0) break;
            used += static_cast<size_t>(got);
        }
    }
    close(fd);
    raw[used] = '\0';

    int status = 0;
    if (std::sscanf(raw, "HTTP/1.%*d %d", &status) == 1) response.status = status;
    if (const char* body = std::strstr(raw, "\r\n\r\n"))
        std::snprintf(response.body, sizeof(response.body), "%s", body + 4);
    // The body can carry handed-over keys.
    OPENSSL_cleanse(raw, sizeof(raw));
    return response;
}

bool http_port_free(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    const int enable = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    const sockaddr_in address = ipv4_address(port, INADDR_ANY);
    const bool available = bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    close(fd);
    return available;
}

bool http_self_test(int port) {
    log_event("HTTP_SELF_TEST_BEGIN", {field("port", port)});
    const LoopbackResponse response = http_loopback_request(port, "GET", "/status");
    char expected[64];
    std::snprintf(expected, sizeof(expected), "\"pid\":%d,", static_cast<int>(getpid()));
    // The pid check proves this process answered, not an older copy sharing the port.
    const bool success = response.status == 200 && std::strstr(response.body, expected) != nullptr;
    log_event("HTTP_SELF_TEST_END", {field("success", success ? "true" : "false"),
        field("status", response.status), field("errno", response.error),
        field("error", response.error ? std::strerror(response.error) : "")});
    return success;
}

void http_wait() {
    std::unique_lock<std::mutex> lock(shutdown_mutex);
    // A periodic entry dates a silent stop to within ten minutes and shows memory growth.
    while (!shutdown_signal.wait_for(lock, std::chrono::minutes(10), [] { return shutdown_requested; })) {
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        lock.unlock();
        log_event("HEARTBEAT", {field("uptime_s", uptime_seconds()), field("requests", request_count.load()),
            field("max_rss_kib", static_cast<long long>(usage.ru_maxrss))});
        lock.lock();
    }
    log_event("HTTP_SHUTDOWN_REQUESTED");
}

void http_stop() {
    request_shutdown();
}

void http_close() {
    if (!server) return;
    closing.store(true);
    {
        std::lock_guard<std::mutex> lock(server_mutex);
        server->stop();
    }
    if (listener.joinable()) listener.join();
    server.reset();
    log_event("HTTP_PORT_RELEASED");
}

}
