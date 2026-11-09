#include "transfer.hpp"

#include <curl/curl.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#ifdef PS5_PAYLOAD
#include <sys/socket.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace archive_catalog {

std::string request_error_message(const std::string& detail, long status, bool credentials_provided) {
    if (status != 401 && status != 403) return detail;
    return detail + (credentials_provided
        ? "; Archive.org denied access: S3 keys were provided. Check them in Settings; this account may lack permission."
        : "; Archive.org denied access: S3 keys may be required. Add them in Settings.");
}
namespace {

using Handle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;

struct MemorySink {
    std::string data;
    std::size_t limit;
};

struct FileSink {
    std::ofstream stream;
    std::uint64_t received;
    std::uint64_t expected;
};

struct SocketInfo {
    int receive_buffer_default = -1;
    int receive_buffer = -1;
};

std::function<void(const TransferReport&)> transfer_observer;

#ifdef PS5_PAYLOAD
// The console's default receive window caps one connection far below the link speed.
int enlarge_receive_buffer(void* opaque, curl_socket_t socket, curlsocktype purpose) noexcept {
    if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKOPT_OK;
    auto& info = *static_cast<SocketInfo*>(opaque);
    socklen_t length = sizeof(info.receive_buffer_default);
    getsockopt(socket, SOL_SOCKET, SO_RCVBUF, &info.receive_buffer_default, &length);
    for (int size : {4 * 1024 * 1024, 2 * 1024 * 1024, 1024 * 1024}) {
        if (setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) == 0) break;
    }
    length = sizeof(info.receive_buffer);
    getsockopt(socket, SOL_SOCKET, SO_RCVBUF, &info.receive_buffer, &length);
    return CURL_SOCKOPT_OK;
}
#endif

TransferReport describe_transfer(CURL* handle, const File& file, const SocketInfo& socket) {
    TransferReport report;
    report.name = file.name;
    report.receive_buffer_default = socket.receive_buffer_default;
    report.receive_buffer = socket.receive_buffer;
    curl_off_t value = 0;
    if (curl_easy_getinfo(handle, CURLINFO_SIZE_DOWNLOAD_T, &value) == CURLE_OK) report.bytes = static_cast<std::uint64_t>(value);
    if (curl_easy_getinfo(handle, CURLINFO_TOTAL_TIME_T, &value) == CURLE_OK) report.total_us = value;
    if (curl_easy_getinfo(handle, CURLINFO_CONNECT_TIME_T, &value) == CURLE_OK) report.connect_us = value;
    if (curl_easy_getinfo(handle, CURLINFO_APPCONNECT_TIME_T, &value) == CURLE_OK) report.tls_us = value;
    curl_easy_getinfo(handle, CURLINFO_HTTP_VERSION, &report.http_version);
    char* text = nullptr;
    if (curl_easy_getinfo(handle, CURLINFO_PRIMARY_IP, &text) == CURLE_OK && text) report.address = text;
    if (curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &text) == CURLE_OK && text) {
        std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
        char* host = nullptr;
        if (parsed && curl_url_set(parsed.get(), CURLUPART_URL, text, 0) == CURLUE_OK &&
            curl_url_get(parsed.get(), CURLUPART_HOST, &host, 0) == CURLUE_OK) {
            report.host = host;
            curl_free(host);
        }
    }
    return report;
}

void report_transfer(const TransferReport& report) {
    if (!transfer_observer) return;
    try { transfer_observer(report); } catch (...) {}
}

struct RequestError : std::runtime_error {
    CURLcode code;
    long status;
    curl_off_t retry_after;
    RequestError(const std::string& message, CURLcode result, long response, curl_off_t retry)
        : std::runtime_error(message), code(result), status(response), retry_after(retry) {}
};

bool retryable(const RequestError& error) {
    if (error.retry_after > 60) return false;
    if (error.code == CURLE_HTTP_RETURNED_ERROR) {
        return error.status == 408 || error.status == 429 || error.status == 500 ||
               error.status == 502 || error.status == 503 || error.status == 504;
    }
    return error.code == CURLE_COULDNT_RESOLVE_HOST || error.code == CURLE_COULDNT_CONNECT ||
           error.code == CURLE_OPERATION_TIMEDOUT || error.code == CURLE_RECV_ERROR ||
           error.code == CURLE_SEND_ERROR || error.code == CURLE_GOT_NOTHING || error.code == CURLE_PARTIAL_FILE;
}

std::size_t memory_write(char* data, std::size_t size, std::size_t count, void* opaque) noexcept {
    auto& sink = *static_cast<MemorySink*>(opaque);
    if (size && count > std::numeric_limits<std::size_t>::max() / size) return 0;
    auto bytes = size * count;
    if (bytes > sink.limit - sink.data.size()) return 0;
    try { sink.data.append(data, bytes); } catch (...) { return 0; }
    return bytes;
}

std::size_t file_write(char* data, std::size_t size, std::size_t count, void* opaque) noexcept {
    auto& sink = *static_cast<FileSink*>(opaque);
    if (size && count > std::numeric_limits<std::size_t>::max() / size) return 0;
    auto bytes = size * count;
    if (bytes > sink.expected - sink.received) return 0;
    sink.stream.write(data, static_cast<std::streamsize>(bytes));
    if (!sink.stream) return 0;
    sink.received += bytes;
    return bytes;
}

template <typename Value>
void option(CURL* handle, CURLoption setting, Value value) {
    auto result = curl_easy_setopt(handle, setting, value);
    if (result != CURLE_OK) throw std::runtime_error(curl_easy_strerror(result));
}

void check_cancelled(const TransferHooks& hooks) {
    if (hooks.cancelled && hooks.cancelled()) throw OperationCancelled();
}

int transfer_progress(void* opaque, curl_off_t total, curl_off_t received, curl_off_t, curl_off_t) noexcept {
    const auto& hooks = *static_cast<const TransferHooks*>(opaque);
    try {
        if (hooks.cancelled && hooks.cancelled()) return 1;
        if (hooks.progress) hooks.progress(static_cast<std::uint64_t>(std::max<curl_off_t>(0, received)),
                                          static_cast<std::uint64_t>(std::max<curl_off_t>(0, total)));
        return 0;
    } catch (...) { return 1; }
}

Handle request(const std::string& url, const std::string& ca_bundle, const TransferHooks& hooks) {
    check_cancelled(hooks);
    item_id(url);
    Handle handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle) throw std::runtime_error("Cannot create HTTP request");
    option(handle.get(), CURLOPT_URL, url.c_str());
    option(handle.get(), CURLOPT_FOLLOWLOCATION, 1L);
    option(handle.get(), CURLOPT_MAXREDIRS, 8L);
    option(handle.get(), CURLOPT_PROTOCOLS_STR, "https");
    option(handle.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
    option(handle.get(), CURLOPT_FAILONERROR, 1L);
    option(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    option(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    option(handle.get(), CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
#ifdef _WIN32
    option(handle.get(), CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
#endif
    option(handle.get(), CURLOPT_CONNECTTIMEOUT, 20L);
    option(handle.get(), CURLOPT_LOW_SPEED_LIMIT, 1024L);
    option(handle.get(), CURLOPT_LOW_SPEED_TIME, 60L);
    option(handle.get(), CURLOPT_NOSIGNAL, 1L);
    option(handle.get(), CURLOPT_USERAGENT, "ArchiveCatalog/0.1 (metadata catalog and selected downloads)");
    if (!ca_bundle.empty()) option(handle.get(), CURLOPT_CAINFO, ca_bundle.c_str());
    option(handle.get(), CURLOPT_NOPROGRESS, 0L);
    option(handle.get(), CURLOPT_XFERINFOFUNCTION, transfer_progress);
    option(handle.get(), CURLOPT_XFERINFODATA, &hooks);
    return handle;
}

int s3_prerequisite(void* opaque, char*, char*, int, int) noexcept {
    auto* handle = static_cast<CURL*>(opaque);
    char* effective_url = nullptr;
    try {
        return curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &effective_url) == CURLE_OK &&
            effective_url && s3_destination_allowed(effective_url) ? CURL_PREREQFUNC_OK : CURL_PREREQFUNC_ABORT;
    } catch (...) { return CURL_PREREQFUNC_ABORT; }
}

using Headers = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

Headers authorize(CURL* handle, const std::string& authorization) {
    Headers headers(nullptr, curl_slist_free_all);
    if (authorization.empty()) return headers;
    headers.reset(curl_slist_append(nullptr, authorization.c_str()));
    if (!headers) throw std::runtime_error("Cannot create authentication header");
    option(handle, CURLOPT_HTTPHEADER, headers.get());
    option(handle, CURLOPT_UNRESTRICTED_AUTH, 1L);
    option(handle, CURLOPT_PREREQFUNCTION, s3_prerequisite);
    option(handle, CURLOPT_PREREQDATA, handle);
    return headers;
}

void check_result(CURL* handle, CURLcode result, const char* error, bool authorized) {
    long status = 0;
    curl_off_t retry_after = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(handle, CURLINFO_RETRY_AFTER, &retry_after);
    if (result != CURLE_OK) {
        auto message = request_error_message(error[0] ? error : curl_easy_strerror(result), status, authorized);
        throw RequestError(message, result, status, retry_after);
    }
    if (status != 200 && status != 206) throw std::runtime_error("Unexpected HTTP status " + std::to_string(status));
}

void perform(CURL* handle, const TransferHooks& hooks, const S3Credentials& s3) {
    const auto authorization = s3.authorization();
    const auto headers = authorize(handle, authorization);
    std::array<char, CURL_ERROR_SIZE> error{};
    option(handle, CURLOPT_ERRORBUFFER, error.data());
    auto result = curl_easy_perform(handle);
    check_cancelled(hooks);
    check_result(handle, result, error.data(), !authorization.empty());
}

void regular_or_missing(const std::filesystem::path& path) {
    auto status = std::filesystem::symlink_status(path);
    if (std::filesystem::is_symlink(status) ||
        (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status))) {
        throw std::runtime_error("Refusing non-regular file: " + path.string());
    }
}

constexpr std::uint64_t segmented_minimum = 32ULL * 1024 * 1024;

// Parts after the first exist from the start of a split download until the parts are joined.
int existing_segments(const std::filesystem::path& partial) {
    int count = 1;
    while (count < max_partial_segments && std::filesystem::exists(std::filesystem::symlink_status(partial_segment(partial, count)))) ++count;
    return count;
}

struct Segment {
    std::uint64_t start = 0;
    std::uint64_t length = 0;
    std::uint64_t resumed = 0;
    std::filesystem::path path;
    std::vector<char> buffer;
    FileSink sink;
    bool range_checked = false;
    bool range_ignored = false;
    Handle handle{nullptr, curl_easy_cleanup};
    Headers headers{nullptr, curl_slist_free_all};
    std::array<char, CURL_ERROR_SIZE> error{};
    SocketInfo socket;
    int attempts = 0;
    bool waiting = false;
    std::chrono::steady_clock::time_point retry_at;
};

std::size_t segment_write(char* data, std::size_t size, std::size_t count, void* opaque) noexcept {
    auto& segment = *static_cast<Segment*>(opaque);
    if (!segment.range_checked) {
        long status = 0;
        curl_easy_getinfo(segment.handle.get(), CURLINFO_RESPONSE_CODE, &status);
        // A 200 reply is the whole file, which would be written at this part's offset.
        if (status != 206) { segment.range_ignored = true; return 0; }
        segment.range_checked = true;
    }
    return file_write(data, size, count, &segment.sink);
}

struct MultiGuard {
    CURLM* multi;
    std::vector<CURL*> added;
    ~MultiGuard() {
        for (auto* handle : added) curl_multi_remove_handle(multi, handle);
        if (multi) curl_multi_cleanup(multi);
    }
};

// Each part resumes from its own file size; the parts are joined into the first one before verification.
void download_segments(const File& file, const std::filesystem::path& partial, int count, const std::string& ca_bundle,
                       const TransferHooks& hooks, const S3Credentials& s3) {
    const auto authorization = s3.authorization();
    TransferHooks quiet;
    quiet.cancelled = hooks.cancelled;
    std::vector<std::unique_ptr<Segment>> segments;
    std::uint64_t remaining = 0;
    for (int index = 0; index < count; ++index) {
        auto segment = std::make_unique<Segment>();
        segment->start = file.size * static_cast<std::uint64_t>(index) / static_cast<std::uint64_t>(count);
        segment->length = file.size * static_cast<std::uint64_t>(index + 1) / static_cast<std::uint64_t>(count) - segment->start;
        segment->path = index ? partial_segment(partial, index) : partial;
        regular_or_missing(segment->path);
        const std::uint64_t existing = std::filesystem::exists(segment->path) ? std::filesystem::file_size(segment->path) : 0;
        // Only the first part can be longer, after an interrupted join; the other parts are intact then.
        if (index && existing > segment->length) throw std::runtime_error("Partial download part is larger than expected; remove " + segment->path.string());
        segment->resumed = std::min(existing, segment->length);
        segment->sink.received = segment->resumed;
        segment->sink.expected = segment->length;
        remaining += segment->length - segment->resumed;
        segments.push_back(std::move(segment));
    }
    if (std::filesystem::space(partial.parent_path()).available < remaining + file.size - segments.front()->length)
        throw std::runtime_error("Insufficient space for " + file.name);
    MultiGuard multi{curl_multi_init(), {}};
    if (!multi.multi) throw std::runtime_error("Cannot create download connections");
    // Each part resumes from what it has written, also when it is restarted after a refusal.
    const auto start = [&](Segment& segment) {
        segment.handle = request(download_url(file), ca_bundle, quiet);
        auto* handle = segment.handle.get();
#ifdef PS5_PAYLOAD
        option(handle, CURLOPT_SOCKOPTFUNCTION, enlarge_receive_buffer);
        option(handle, CURLOPT_SOCKOPTDATA, &segment.socket);
#endif
        option(handle, CURLOPT_BUFFERSIZE, 512L * 1024L);
        const auto range = std::to_string(segment.start + segment.sink.received) + "-" + std::to_string(segment.start + segment.length - 1);
        option(handle, CURLOPT_RANGE, range.c_str());
        option(handle, CURLOPT_WRITEFUNCTION, segment_write);
        option(handle, CURLOPT_WRITEDATA, &segment);
        option(handle, CURLOPT_ERRORBUFFER, segment.error.data());
        segment.error[0] = 0;
        segment.range_checked = false;
        segment.headers = authorize(handle, authorization);
        if (curl_multi_add_handle(multi.multi, handle) != CURLM_OK) throw std::runtime_error("Cannot start a download connection");
        multi.added.push_back(handle);
    };
    for (auto& segment : segments) {
        if (segment->resumed == segment->length) continue;
        segment->buffer.resize(1024 * 1024);
        segment->sink.stream.rdbuf()->pubsetbuf(segment->buffer.data(), static_cast<std::streamsize>(segment->buffer.size()));
        segment->sink.stream.open(segment->path, std::ios::binary | std::ios::app);
        if (!segment->sink.stream) throw std::runtime_error("Cannot write " + segment->path.string());
        start(*segment);
    }
    const auto started = std::chrono::steady_clock::now();
    int running = static_cast<int>(multi.added.size());
    const auto waiting = [&] { return std::any_of(segments.begin(), segments.end(), [](const auto& segment) { return segment->waiting; }); };
    while (running || waiting()) {
        const auto now = std::chrono::steady_clock::now();
        for (auto& segment : segments)
            if (segment->waiting && now >= segment->retry_at) { segment->waiting = false; start(*segment); }
        if (curl_multi_perform(multi.multi, &running) != CURLM_OK) throw std::runtime_error("Download connections failed");
        int queued = 0;
        while (auto* message = curl_multi_info_read(multi.multi, &queued)) {
            if (message->msg != CURLMSG_DONE) continue;
            check_cancelled(hooks);
            const auto found = std::find_if(segments.begin(), segments.end(), [&](const auto& segment) { return segment->handle.get() == message->easy_handle; });
            if (found == segments.end()) continue;
            auto& segment = **found;
            if (segment.range_ignored) throw std::runtime_error("Archive.org ignored a byte range request for " + file.name);
            try {
                check_result(message->easy_handle, message->data.result, segment.error.data(), !authorization.empty());
            } catch (const RequestError& error) {
                // A refused or dropped part waits and continues on its own; the other parts keep going.
                if (!retryable(error) || segment.attempts >= 5) throw;
                const auto delay = std::max<curl_off_t>(curl_off_t{1} << segment.attempts, error.retry_after);
                ++segment.attempts;
                std::cerr << "Download part of " << file.name << " failed: " << error.what() << "; retrying in " << delay << " seconds\n";
                auto* handle = segment.handle.get();
                curl_multi_remove_handle(multi.multi, handle);
                multi.added.erase(std::remove(multi.added.begin(), multi.added.end(), handle), multi.added.end());
                segment.waiting = true;
                segment.retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(delay);
            }
        }
        std::uint64_t received = 0;
        for (const auto& segment : segments) received += segment->sink.received;
        if (hooks.progress) hooks.progress(received, file.size);
        check_cancelled(hooks);
        if ((running || waiting()) && curl_multi_poll(multi.multi, nullptr, 0, 250, nullptr) != CURLM_OK) throw std::runtime_error("Download connections failed");
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    bool html = false;
    std::uint64_t transferred = 0;
    const Segment* first = nullptr;
    for (auto& segment : segments) {
        if (!segment->handle) continue;
        segment->sink.stream.close();
        if (!segment->sink.stream) throw std::runtime_error("Cannot close " + segment->path.string());
        if (segment->sink.received != segment->length) throw std::runtime_error("A download connection ended early for " + file.name);
        char* content_type = nullptr;
        curl_easy_getinfo(segment->handle.get(), CURLINFO_CONTENT_TYPE, &content_type);
        if (content_type && std::string(content_type).find("text/html") != std::string::npos) html = true;
        transferred += segment->sink.received - segment->resumed;
        if (!first) first = segment.get();
    }
    if (html) {
        for (const auto& segment : segments) std::filesystem::remove(segment->path);
        throw std::runtime_error("Archive.org returned an HTML page instead of the selected file; check your IA-S3 keys and download permission");
    }
    if (first) {
        auto report = describe_transfer(first->handle.get(), file, first->socket);
        report.bytes = transferred;
        report.total_us = elapsed;
        report.connections = static_cast<int>(multi.added.size());
        report_transfer(report);
    }
    std::filesystem::resize_file(partial, segments.front()->length);
    {
        std::uint64_t joined = 0, join_total = 0;
        for (std::size_t index = 1; index < segments.size(); ++index) join_total += segments[index]->length;
        if (hooks.stage) hooks.stage("Joining parts", 0, join_total);
        std::vector<char> copy(1024 * 1024);
        std::ofstream output(partial, std::ios::binary | std::ios::app);
        for (std::size_t index = 1; index < segments.size(); ++index) {
            std::ifstream input(segments[index]->path, std::ios::binary);
            while (input.read(copy.data(), static_cast<std::streamsize>(copy.size())) || input.gcount()) {
                // Safe to stop here: the next attempt cuts the first part back to its length and joins again.
                check_cancelled(hooks);
                output.write(copy.data(), input.gcount());
                joined += static_cast<std::uint64_t>(input.gcount());
                if (hooks.stage) hooks.stage("Joining parts", joined, join_total);
            }
            if (!input.eof() || !output) throw std::runtime_error("Cannot join the downloaded parts of " + file.name);
        }
        output.close();
        if (!output) throw std::runtime_error("Cannot join the downloaded parts of " + file.name);
    }
    if (std::filesystem::file_size(partial) != file.size) throw std::runtime_error("Joined download has the wrong size: " + file.name);
    for (std::size_t index = 1; index < segments.size(); ++index) std::filesystem::remove(segments[index]->path);
}

}

std::string S3Credentials::authorization() const {
    if (access_key.empty() && secret_key.empty()) return {};
    if (access_key.empty() || secret_key.empty()) throw std::runtime_error("Enter both IA-S3 keys");
    for (const auto* key : {&access_key, &secret_key}) {
        if (key->size() > 1024) throw std::runtime_error("IA-S3 key is too long");
        for (unsigned char character : *key) {
            if (character <= 32 || character >= 127 || character == ':') {
                throw std::runtime_error("IA-S3 keys contain invalid characters");
            }
        }
    }
    return "Authorization: LOW " + access_key + ":" + secret_key;
}

bool s3_destination_allowed(const std::string& url) {
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
    if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) return false;
    auto part = [&](CURLUPart component) {
        char* value = nullptr;
        auto result = curl_url_get(parsed.get(), component, &value, 0);
        std::unique_ptr<char, decltype(&curl_free)> owned(value, curl_free);
        return result == CURLUE_OK ? std::string(value) : std::string{};
    };
    auto host = part(CURLUPART_HOST);
    for (auto& character : host) {
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    }
    const std::string suffix = ".archive.org";
    return part(CURLUPART_SCHEME) == "https" && (part(CURLUPART_PORT).empty() || part(CURLUPART_PORT) == "443") &&
        part(CURLUPART_USER).empty() && part(CURLUPART_PASSWORD).empty() &&
        (host == "archive.org" || (host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0));
}

void set_transfer_observer(std::function<void(const TransferReport&)> observer) { transfer_observer = std::move(observer); }

std::filesystem::path partial_segment(const std::filesystem::path& partial, int index) {
    auto path = partial;
    path += "." + std::to_string(index);
    return path;
}

Http::Http(std::string ca_bundle, TransferHooks hooks, S3Credentials s3, int connections)
    : ca_bundle_(std::move(ca_bundle)), hooks_(std::move(hooks)), s3_(std::move(s3)),
      connections_(std::clamp(connections, 1, max_download_connections)) {
    s3_.authorization();
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("Cannot initialize curl");
}

Http::~Http() { curl_global_cleanup(); }

std::string Http::get(const std::string& url, std::size_t limit) const {
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto handle = request(url, ca_bundle_, hooks_);
        MemorySink sink{{}, limit};
        option(handle.get(), CURLOPT_TIMEOUT, 120L);
        option(handle.get(), CURLOPT_WRITEFUNCTION, memory_write);
        option(handle.get(), CURLOPT_WRITEDATA, &sink);
        try {
            perform(handle.get(), hooks_, s3_);
            return sink.data;
        } catch (const RequestError& error) {
            if (attempt == 2 || !retryable(error)) throw;
            auto delay = std::max<curl_off_t>(1 << attempt, error.retry_after);
            std::cerr << "Metadata request failed for " << url << ": " << error.what()
                      << "; retrying in " << delay << " seconds\n";
            for (curl_off_t tick = 0; tick < delay * 10; ++tick) {
                check_cancelled(hooks_);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
    throw std::runtime_error("Metadata retry limit exceeded");
}

void verify_file(const File& file, const std::filesystem::path& path, const TransferHooks& hooks) {
    check_cancelled(hooks);
    if (std::filesystem::file_size(path) != file.size) throw std::runtime_error("Size mismatch: " + file.name);
    const auto& expected = file.sha1.empty() ? file.md5 : file.sha1;
    if (expected.empty()) throw std::runtime_error("No checksum available for " + file.name);
    auto* algorithm = file.sha1.empty() ? EVP_md5() : EVP_sha1();
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), algorithm, nullptr) != 1) throw std::runtime_error("Cannot initialize checksum verification");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read " + path.string());
    // On the heap: PS5 payload threads have small default stacks.
    std::vector<char> buffer(128 * 1024);
    std::uint64_t checked = 0, reported = 0;
    if (hooks.stage) hooks.stage("Verifying", 0, file.size);
    while (input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || input.gcount()) {
        check_cancelled(hooks);
        if (EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(input.gcount())) != 1) throw std::runtime_error("Checksum update failed");
        checked += static_cast<std::uint64_t>(input.gcount());
        if (hooks.stage && (checked - reported >= 4 * 1024 * 1024 || checked == file.size)) {
            reported = checked;
            hooks.stage("Verifying", checked, file.size);
        }
    }
    if (!input.eof()) throw std::runtime_error("Read failure during checksum verification");
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1) throw std::runtime_error("Checksum verification failed");
    const char* hexadecimal = "0123456789abcdef";
    std::string actual;
    for (unsigned int index = 0; index < length; ++index) {
        actual += hexadecimal[digest[index] >> 4];
        actual += hexadecimal[digest[index] & 15];
    }
    if (actual != expected) throw std::runtime_error("Checksum mismatch: " + file.name);
}

std::filesystem::path download_target(const std::filesystem::path& directory, const File& file) {
    download_url(file);
    std::filesystem::create_directories(directory);
    auto target = std::filesystem::canonical(directory) / std::filesystem::u8path(file.name).filename();
    regular_or_missing(target);
    regular_or_missing(target.string() + ".part");
    return target;
}

void Http::download(const File& file, const std::filesystem::path& directory, bool overwrite) const {
    check_cancelled(hooks_);
    if (file.sha1.empty() && file.md5.empty()) throw std::runtime_error("Refusing download without a manifest checksum: " + file.name);
    if (file.size > static_cast<std::uint64_t>(std::numeric_limits<curl_off_t>::max())) throw std::runtime_error("File exceeds supported transfer size");
    auto target = download_target(directory, file);
    if (!overwrite && std::filesystem::exists(target)) {
        verify_file(file, target, hooks_);
        std::cout << "Already verified: " << file.name << '\n';
        return;
    }
    auto partial = std::filesystem::path(target.string() + ".part");
    const int segments = existing_segments(partial);
    auto offset = std::filesystem::exists(partial) ? std::filesystem::file_size(partial) : 0;
    if (segments > 1 || (connections_ > 1 && offset == 0 && file.size >= segmented_minimum)) {
        download_segments(file, partial, segments > 1 ? segments : connections_, ca_bundle_, hooks_, s3_);
        offset = file.size;
    }
    if (offset > file.size) throw std::runtime_error("Partial file is larger than manifest size; remove " + partial.string());
    auto space = std::filesystem::space(target.parent_path());
    if (space.available < file.size - offset) throw std::runtime_error("Insufficient space for " + file.name);
    if (offset < file.size) {
        TransferHooks adjusted = hooks_;
        adjusted.progress = [progress = hooks_.progress, offset, expected = file.size](std::uint64_t received, std::uint64_t) {
            if (progress) progress(std::min(expected, offset + received), expected);
        };
        auto handle = request(download_url(file), ca_bundle_, adjusted);
        std::vector<char> write_buffer(1024 * 1024);
        FileSink sink{std::ofstream(), offset, file.size};
        // Set before open: curl delivers small chunks, and each would otherwise become its own write.
        sink.stream.rdbuf()->pubsetbuf(write_buffer.data(), static_cast<std::streamsize>(write_buffer.size()));
        sink.stream.open(partial, std::ios::binary | std::ios::app);
        if (!sink.stream) throw std::runtime_error("Cannot write " + partial.string());
        SocketInfo socket;
#ifdef PS5_PAYLOAD
        option(handle.get(), CURLOPT_SOCKOPTFUNCTION, enlarge_receive_buffer);
        option(handle.get(), CURLOPT_SOCKOPTDATA, &socket);
#endif
        option(handle.get(), CURLOPT_BUFFERSIZE, 512L * 1024L);
        option(handle.get(), CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(offset));
        option(handle.get(), CURLOPT_WRITEFUNCTION, file_write);
        option(handle.get(), CURLOPT_WRITEDATA, &sink);
        perform(handle.get(), adjusted, s3_);
        report_transfer(describe_transfer(handle.get(), file, socket));
        sink.stream.flush();
        if (!sink.stream) throw std::runtime_error("Cannot flush " + partial.string());
        sink.stream.close();
        if (!sink.stream) throw std::runtime_error("Cannot close " + partial.string());
        char* content_type = nullptr;
        curl_easy_getinfo(handle.get(), CURLINFO_CONTENT_TYPE, &content_type);
        if (content_type && std::string(content_type).find("text/html") != std::string::npos) {
            std::filesystem::remove(partial);
            throw std::runtime_error("Archive.org returned an HTML page instead of the selected file; check your IA-S3 keys and download permission");
        }
    }
    try { verify_file(file, partial, hooks_); }
    catch (const OperationCancelled&) { throw; }
    catch (...) {
        std::filesystem::remove(partial);
        throw;
    }
    check_cancelled(hooks_);
    regular_or_missing(target);
#ifdef _WIN32
    if (!MoveFileExW(partial.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0))) {
        throw std::filesystem::filesystem_error("Cannot publish verified download", partial, target,
                                               std::error_code(GetLastError(), std::system_category()));
    }
#else
    if (!overwrite && std::filesystem::exists(target)) throw std::runtime_error("Destination already exists: " + target.string());
    std::filesystem::rename(partial, target);
#endif
    std::cout << "Verified: " << target.string() << '\n';
}

}