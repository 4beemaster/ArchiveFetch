#include "diag_log.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(PS5_PAYLOAD)
#include <dlfcn.h>
#endif

#include "diag_config.hpp"

namespace archivefetch_diag {
namespace {

int log_fd = -1;
bool clock_started = false;
timespec started{};
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void start_clock() {
    if (!clock_started) {
        clock_gettime(CLOCK_MONOTONIC, &started);
        clock_started = true;
    }
}

long long elapsed_ms() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - started.tv_sec) * 1000LL + (now.tv_nsec - started.tv_nsec) / 1000000LL;
}

void local_timestamp(char* out, size_t size) {
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    tm local{};
    char date[32] = "";
    char zone[16] = "";
    if (localtime_r(&now.tv_sec, &local)) {
        std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &local);
        std::strftime(zone, sizeof(zone), "%z", &local);
    }
    std::snprintf(out, size, "%s.%03ld%s", date, static_cast<long>(now.tv_nsec / 1000000L), zone);
}

class Line {
public:
    // Space kept back so the closing braces always fit after a dropped field.
    static constexpr size_t reserve = 64;

    void raw(const char* text, size_t length) {
        if (overflow_ || used_ + length > sizeof(data_) - reserve) {
            overflow_ = true;
            return;
        }
        std::memcpy(data_ + used_, text, length);
        used_ += length;
    }
    void raw(const char* text) { raw(text, std::strlen(text)); }

    void string(const char* text) {
        raw("\"", 1);
        for (const char* p = text ? text : ""; *p; ++p) {
            const auto c = static_cast<unsigned char>(*p);
            if (c == '"' || c == '\\') {
                const char escaped[2] = {'\\', static_cast<char>(c)};
                raw(escaped, 2);
            } else if (c < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                raw(escaped);
            } else {
                raw(p, 1);
            }
        }
        raw("\"", 1);
    }

    void integer(long long value) {
        char text[32];
        std::snprintf(text, sizeof(text), "%lld", value);
        raw(text);
    }

    size_t mark() const { return used_; }
    bool overflowed() const { return overflow_; }
    void rewind(size_t mark) {
        used_ = mark;
        overflow_ = false;
    }

    void finish(const char* text) {
        const size_t length = std::strlen(text);
        if (used_ + length <= sizeof(data_)) {
            std::memcpy(data_ + used_, text, length);
            used_ += length;
        }
    }

    const char* data() const { return data_; }
    size_t size() const { return used_; }

private:
    char data_[4096];
    size_t used_ = 0;
    bool overflow_ = false;
};

void write_all(int fd, const char* data, size_t size) {
    while (size > 0) {
        const ssize_t written = write(fd, data, size);
        if (written < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (written == 0) return;
        data += written;
        size -= static_cast<size_t>(written);
    }
}

}

bool log_open(const char* directory, const char* path, int* error_out) {
    start_clock();
    if (mkdir(directory, 0777) != 0 && errno != EEXIST) {
        if (error_out) *error_out = errno;
        return false;
    }
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (fd < 0) {
        if (error_out) *error_out = errno;
        return false;
    }
    pthread_mutex_lock(&log_mutex);
    log_fd = fd;
    pthread_mutex_unlock(&log_mutex);
    return true;
}

void log_event(const char* event, std::initializer_list<Field> details) {
    const int saved_errno = errno;
    start_clock();
    char timestamp[64];
    local_timestamp(timestamp, sizeof(timestamp));

    Line line;
    line.raw("{\"event\":");
    line.string(event);
    line.raw(",\"version\":");
    line.string(version);
    line.raw(",\"pid\":");
    line.integer(getpid());
    line.raw(",\"elapsed_ms\":");
    line.integer(elapsed_ms());
    line.raw(",\"local_timestamp\":");
    line.string(timestamp);
    line.raw(",\"details\":{");

    bool first = true;
    bool dropped = false;
    for (const Field& item : details) {
        const size_t mark = line.mark();
        if (!first) line.raw(",");
        line.string(item.key);
        line.raw(":");
        if (item.kind == Field::Kind::Text) {
            line.string(item.text);
        } else if (item.kind == Field::Kind::Integer) {
            line.integer(item.integer);
        } else {
            char hex[16];
            std::snprintf(hex, sizeof(hex), "0x%08x", static_cast<unsigned>(item.integer));
            line.string(hex);
        }
        if (line.overflowed()) {
            line.rewind(mark);
            dropped = true;
        } else {
            first = false;
        }
    }
    line.finish(dropped ? "},\"details_truncated\":true}\n" : "}}\n");

    pthread_mutex_lock(&log_mutex);
    const int fd = log_fd >= 0 ? log_fd : STDERR_FILENO;
    write_all(fd, line.data(), line.size());
    if (fd != STDERR_FILENO) fsync(fd);
    pthread_mutex_unlock(&log_mutex);
    errno = saved_errno;
}

namespace {

char alternate_stack[64 * 1024];

void append(char* out, size_t& used, size_t capacity, const char* text) {
    while (*text && used + 1 < capacity) out[used++] = *text++;
}

void append_number(char* out, size_t& used, size_t capacity, unsigned long long value, unsigned base) {
    char digits[24];
    size_t count = 0;
    do {
        digits[count++] = "0123456789abcdef"[value % base];
        value /= base;
    } while (value && count < sizeof(digits));
    while (count && used + 1 < capacity) out[used++] = digits[--count];
}

const char* signal_name(int signal) {
    switch (signal) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        case SIGABRT: return "SIGABRT";
        default: return "other";
    }
}

// Only write, fsync and _exit: the crashed thread may hold the log mutex or the allocator lock.
void on_fatal_signal(int signal, siginfo_t* info, void*) {
    char line[320];
    size_t used = 0;
    append(line, used, sizeof(line), "{\"event\":\"FATAL_SIGNAL\",\"version\":\"");
    append(line, used, sizeof(line), version);
    append(line, used, sizeof(line), "\",\"pid\":");
    append_number(line, used, sizeof(line), static_cast<unsigned long long>(getpid()), 10);
    append(line, used, sizeof(line), ",\"elapsed_ms\":");
    append_number(line, used, sizeof(line), static_cast<unsigned long long>(elapsed_ms()), 10);
    append(line, used, sizeof(line), ",\"details\":{\"signal\":");
    append_number(line, used, sizeof(line), static_cast<unsigned long long>(signal), 10);
    append(line, used, sizeof(line), ",\"name\":\"");
    append(line, used, sizeof(line), signal_name(signal));
    append(line, used, sizeof(line), "\",\"code\":");
    append_number(line, used, sizeof(line), static_cast<unsigned long long>(info ? info->si_code : 0), 10);
    append(line, used, sizeof(line), ",\"address\":\"0x");
    append_number(line, used, sizeof(line), reinterpret_cast<unsigned long long>(info ? info->si_addr : nullptr), 16);
    append(line, used, sizeof(line), "\"}}\n");
    const int fd = log_fd >= 0 ? log_fd : STDERR_FILENO;
    write_all(fd, line, used);
    fsync(fd);
    // Re-raising lets the console's crash handling run, which can leave the process wedged on the port.
    _exit(128 + signal);
}

}

void log_install_crash_handler() {
    stack_t stack{};
    stack.ss_sp = alternate_stack;
    stack.ss_size = sizeof(alternate_stack);
    const bool alternate = sigaltstack(&stack, nullptr) == 0;
    struct sigaction action{};
    action.sa_sigaction = on_fatal_signal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    int installed = 0;
    for (int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) installed += sigaction(signal, &action, nullptr) == 0;
    size_t default_stack = 0;
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) == 0) {
        pthread_attr_getstacksize(&attributes, &default_stack);
        pthread_attr_destroy(&attributes);
    }
    log_event("CRASH_HANDLER", {field("signals", installed), field("alternate_stack", alternate ? "true" : "false"),
        field("default_thread_stack", static_cast<long long>(default_stack))});
}

void log_close() {
    pthread_mutex_lock(&log_mutex);
    if (log_fd >= 0) {
        fsync(log_fd);
        close(log_fd);
        log_fd = -1;
    }
    pthread_mutex_unlock(&log_mutex);
}

long long log_size(const char* path) {
    struct stat info{};
    return stat(path, &info) == 0 ? static_cast<long long>(info.st_size) : 0;
}

long long log_rotate(const char* path, const char* previous, long long limit) {
    const long long size = log_size(path);
    if (size <= limit) return 0;
    return rename(path, previous) == 0 ? size : -1;
}

bool log_clear() {
    pthread_mutex_lock(&log_mutex);
    // The file is opened with O_APPEND, so writes after the truncate start at offset 0.
    const bool cleared = log_fd >= 0 && ftruncate(log_fd, 0) == 0 && fsync(log_fd) == 0;
    pthread_mutex_unlock(&log_mutex);
    return cleared;
}

NotifyResult notify(const char* text) {
#if defined(PS5_PAYLOAD)
    struct Request {
        char reserved[45];
        char message[3075];
    } request{};
    static_assert(sizeof(Request) == 3120, "Notification request layout");
    using Sender = int (*)(int, const void*, size_t, int);
    const auto sender = reinterpret_cast<Sender>(dlsym(RTLD_DEFAULT, "sceKernelSendNotificationRequest"));
    if (!sender) return {false, -1};
    std::snprintf(request.message, sizeof(request.message), "ArchiveFetch: %s", text);
    return {true, sender(0, &request, sizeof(request), 0)};
#else
    (void)text;
    return {false, 0};
#endif
}

}
