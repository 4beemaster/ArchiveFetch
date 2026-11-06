#include "diag_httplib.hpp"

// WebApplication, the shared backend, is header-only.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "web_application.hpp"
#pragma clang diagnostic pop

#include "api_routes.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <set>
#include <sys/socket.h>
#include <sys/stat.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <string>
#include <unistd.h>
#include <vector>
#ifdef PS5_PAYLOAD
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/ucred.h>
#endif

#include "diag_config.hpp"
#include "ca_bundle.hpp"
#include "diag_log.hpp"
#include "extract_runner.hpp"

namespace archivefetch_diag {
namespace {

constexpr int denial_log_limit = 20;

std::mutex application_mutex;
std::unique_ptr<DiagnosticLog> diagnostics;
std::unique_ptr<WebApplication> application;
std::string session_token;
std::atomic<int> denials_logged{0};
// Off until the saved setting is loaded, so outside peers are refused from the start.
std::atomic<bool> external_access{false};
std::filesystem::path network_settings;

// Outside devices sign in with a password; local devices never need it.
constexpr int password_iterations = 100000;
constexpr std::size_t password_min = 8, password_max = 128, grant_limit = 32;
constexpr long long grant_seconds = 30LL * 24 * 3600;
constexpr int peer_failures = 5, global_failures = 20;
constexpr long long peer_lock_seconds = 15 * 60, global_lock_seconds = 3600;
constexpr const char* auth_cookie = "ArchiveFetchAuth";
struct Password { std::string salt, hash; int iterations = 0; };
struct Failures { int count = 0; long long locked_until = 0; };
std::mutex auth_mutex;
Password password;
// Keyed by the SHA-256 of the cookie value, so the saved file cannot be replayed as a cookie.
std::map<std::string, long long> grants;
std::filesystem::path grants_path;
std::map<std::string, Failures> failures;
std::deque<long long> recent_failures;
long long global_lock_until = 0;
// Paired apps hold a permanent key; only its SHA-256 is saved. Keyed by the device id.
struct PairedDevice { std::string name, digest; long long created = 0, last_seen = 0; };
constexpr std::size_t paired_limit = 32;
constexpr const char* device_header = "X-ArchiveFetch-Device";
std::map<std::string, PairedDevice> paired;
std::filesystem::path paired_path;

long long now_seconds() { return static_cast<long long>(std::time(nullptr)); }

std::string hex(const unsigned char* data, std::size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (std::size_t index = 0; index < size; ++index) { text += digits[data[index] >> 4]; text += digits[data[index] & 15]; }
    return text;
}

std::string derive_password(const std::string& text, const std::string& salt, int iterations) {
    unsigned char key[32];
    if (PKCS5_PBKDF2_HMAC(text.data(), static_cast<int>(text.size()), reinterpret_cast<const unsigned char*>(salt.data()), static_cast<int>(salt.size()),
            iterations, EVP_sha256(), sizeof(key), key) != 1) throw std::runtime_error("Cannot check the password");
    const auto result = hex(key, sizeof(key));
    OPENSSL_cleanse(key, sizeof(key));
    return result;
}

bool password_set() {
    std::lock_guard<std::mutex> lock(auth_mutex);
    return !password.hash.empty();
}

std::string grant_digest(const std::string& token) {
    unsigned char digest[32];
    unsigned int size = 0;
    if (EVP_Digest(token.data(), token.size(), digest, &size, EVP_sha256(), nullptr) != 1 || size != sizeof(digest))
        throw std::runtime_error("Cannot check the sign-in");
    return hex(digest, sizeof(digest));
}

// Caller holds auth_mutex. A failed save only costs a sign-in after the next restart.
void save_grants() {
    Json saved = Json::array();
    for (const auto& grant : grants) saved.push_back({{"digest", grant.first}, {"expires", grant.second}});
    try {
        write_download_state(grants_path, {{"version", 1}, {"grants", saved}});
        chmod(grants_path.c_str(), 0600);
    } catch (const std::exception& failure) {
        log_event("SIGN_INS_SAVE_FAILED", {field("error", failure.what())});
    }
}

void load_grants() {
    try {
        if (!std::filesystem::exists(grants_path)) return;
        const auto saved = read_download_state(grants_path);
        const auto now = now_seconds();
        std::lock_guard<std::mutex> lock(auth_mutex);
        for (const auto& grant : saved.at("grants")) {
            const auto digest = grant.at("digest").get<std::string>();
            const auto expires = grant.at("expires").get<long long>();
            if (digest.size() == 64 && expires > now && expires <= now + grant_seconds && grants.size() < grant_limit) grants[digest] = expires;
        }
    } catch (const std::exception& failure) {
        log_event("SIGN_INS_INVALID", {field("error", failure.what())});
    }
}

void save_network_settings() {
    Json value = {{"external", external_access.load()}};
    {
        std::lock_guard<std::mutex> lock(auth_mutex);
        if (!password.hash.empty()) value["password"] = {{"salt", password.salt}, {"hash", password.hash}, {"iterations", password.iterations}};
    }
    write_download_state(network_settings, value);
}

std::string presented_grant(const httplib::Request& request) {
    const auto cookies = request.get_header_value("Cookie");
    const std::string name = std::string(auth_cookie) + "=";
    for (std::size_t start = 0; start < cookies.size();) {
        while (start < cookies.size() && (cookies[start] == ' ' || cookies[start] == ';')) ++start;
        const auto end = std::min(cookies.find(';', start), cookies.size());
        if (cookies.compare(start, name.size(), name) == 0) return cookies.substr(start + name.size(), end - start - name.size());
        start = end;
    }
    return {};
}

bool signed_in(const httplib::Request& request) {
    const auto presented = presented_grant(request);
    if (presented.empty() || presented.size() > 128) return false;
    const auto digest = grant_digest(presented);
    std::lock_guard<std::mutex> lock(auth_mutex);
    const auto now = now_seconds();
    for (auto grant = grants.begin(); grant != grants.end();) {
        if (grant->second <= now) { grant = grants.erase(grant); continue; }
        if (CRYPTO_memcmp(grant->first.data(), digest.data(), digest.size()) == 0) return true;
        ++grant;
    }
    return false;
}

bool hex_text(const std::string& text, std::size_t size) {
    return text.size() == size && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

// Caller holds auth_mutex.
void save_paired() {
    Json saved = Json::array();
    for (const auto& device : paired)
        saved.push_back({{"id", device.first}, {"name", device.second.name}, {"digest", device.second.digest},
            {"created", device.second.created}, {"lastSeen", device.second.last_seen}});
    try {
        write_download_state(paired_path, {{"version", 1}, {"devices", saved}});
        chmod(paired_path.c_str(), 0600);
    } catch (const std::exception& failure) {
        log_event("PAIRED_DEVICES_SAVE_FAILED", {field("error", failure.what())});
    }
}

void load_paired() {
    try {
        if (!std::filesystem::exists(paired_path)) return;
        const auto saved = read_download_state(paired_path);
        std::lock_guard<std::mutex> lock(auth_mutex);
        for (const auto& device : saved.at("devices")) {
            const auto id = device.at("id").get<std::string>(), digest = device.at("digest").get<std::string>();
            if (hex_text(id, 16) && hex_text(digest, 64) && paired.size() < paired_limit)
                paired[id] = {device.at("name").get<std::string>().substr(0, 64), digest, device.value("created", 0LL), device.value("lastSeen", 0LL)};
        }
    } catch (const std::exception& failure) {
        log_event("PAIRED_DEVICES_INVALID", {field("error", failure.what())});
    }
}

// The header is "id:key"; returns the paired device id, or empty when the header is missing or not paired.
std::string paired_device(const httplib::Request& request) {
    const auto header = request.get_header_value(device_header);
    const auto colon = header.find(':');
    if (colon != 16 || !hex_text(header.substr(0, 16), 16) || !hex_text(header.substr(17), 64)) return {};
    const auto id = header.substr(0, 16);
    const auto digest = grant_digest(header.substr(17));
    std::lock_guard<std::mutex> lock(auth_mutex);
    const auto found = paired.find(id);
    if (found == paired.end() || CRYPTO_memcmp(found->second.digest.data(), digest.data(), digest.size()) != 0) return {};
    // Last use is saved at most hourly so a busy app does not rewrite the file on every request.
    const auto now = now_seconds();
    if (now - found->second.last_seen >= 3600) { found->second.last_seen = now; save_paired(); }
    return id;
}

// Loopback, private (RFC 1918) and link-local addresses: devices on the console's own network.
bool local_peer(const std::string& address) {
    in_addr parsed{};
    if (inet_pton(AF_INET, address.c_str(), &parsed) != 1) return false;
    const auto ip = ntohl(parsed.s_addr);
    return (ip >> 24) == 127 || (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 16) == 0xA9FE;
}

bool host_allowed(const std::string& host) {
    const std::string suffix = ":" + std::to_string(http_port);
    if (host.size() <= suffix.size() || host.compare(host.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    const std::string name = host.substr(0, host.size() - suffix.size());
    if (name == "localhost") return true;
    in_addr parsed{};
    // Only IP literals: a DNS name resolving here (DNS rebinding) is refused.
    return inet_pton(AF_INET, name.c_str(), &parsed) == 1;
}

bool session_matches(const std::string& presented) {
    return !session_token.empty() && presented.size() == session_token.size() &&
        CRYPTO_memcmp(presented.data(), session_token.data(), session_token.size()) == 0;
}

const char* denial(const httplib::Request& request, bool needs_session) {
    const std::string host = request.get_header_value("Host");
    if (!host_allowed(host)) return "host";
    if (request.has_header("Origin") && request.get_header_value("Origin") != "http://" + host) return "origin";
    if (request.get_header_value("Sec-Fetch-Site") == "cross-site") return "cross-site";
    if (needs_session && !session_matches(request.get_header_value("X-Archive-Session"))) return "session";
    return nullptr;
}

void set_json(httplib::Response& response, const std::string& body) {
    response.set_content(body, "application/json; charset=utf-8");
}

std::map<std::uint64_t, std::string> logged_queue;
std::string logged_notice;
std::map<std::string, std::string> logged_source_errors;
std::set<std::string> logged_agents;

// LAN addresses another device can open; loopback is skipped because it only works on the console.
Json network_urls(const std::string& local_address) {
    std::set<std::string> urls;
    const auto add = [&](const std::string& address) {
        if (!address.empty() && address != "0.0.0.0" && address.rfind("127.", 0) != 0)
            urls.insert("http://" + address + ":" + std::to_string(http_port) + "/");
    };
    add(local_address);
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        for (auto* entry = interfaces; entry; entry = entry->ifa_next) {
            if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET) continue;
            char text[INET_ADDRSTRLEN]{};
            if (inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)->sin_addr, text, sizeof(text))) add(text);
        }
        freeifaddrs(interfaces);
    }
    // A UDP connect sends nothing; it only selects the interface used for the default route.
    const int probe = socket(AF_INET, SOCK_DGRAM, 0);
    if (probe >= 0) {
        sockaddr_in route{};
        route.sin_family = AF_INET;
        route.sin_port = htons(9);
        inet_pton(AF_INET, "192.0.2.1", &route.sin_addr);
        sockaddr_in address{};
        socklen_t length = sizeof(address);
        char text[INET_ADDRSTRLEN]{};
        if (connect(probe, reinterpret_cast<sockaddr*>(&route), sizeof(route)) == 0 &&
            getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length) == 0 &&
            inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text))) add(text);
        close(probe);
    }
    return {{"urls", std::vector<std::string>(urls.begin(), urls.end())}};
}

// Logged per status and per received quarter; "Active" at 4/4 means the file is being verified.
void log_queue_changes(const Json& state) {
    std::map<std::uint64_t, std::string> current;
    for (const auto& entry : state.at("queue")) {
        const auto id = entry.at("id").get<std::uint64_t>();
        const auto received = entry.at("received").get<std::uint64_t>(), size = entry.at("size").get<std::uint64_t>();
        const auto quarter = size ? received * 4 / size : 4;
        const auto stage = entry.value("stage", std::string{});
        auto key = entry.at("status").get<std::string>() + ":" + std::to_string(quarter) + ":" + stage;
        const auto previous = logged_queue.find(id);
        if (previous == logged_queue.end() || previous->second != key)
            log_event("DOWNLOAD_STATE", {field("id", static_cast<long long>(id)), field("name", entry.at("name").get<std::string>().c_str()),
                field("status", entry.at("status").get<std::string>().c_str()), field("quarter", static_cast<long long>(quarter)),
                field("stage", stage.c_str()),
                field("received", static_cast<long long>(received)), field("size", static_cast<long long>(size)),
                field("detail", entry.at("detail").get<std::string>().c_str()),
                field("destination", entry.at("destination").get<std::string>().c_str())});
        current.emplace(id, std::move(key));
    }
    logged_queue = std::move(current);
}

// Scan results only reach the browser as a notice, so they are copied into the log when they change.
void log_scan_changes(const Json& state) {
    const auto notice = state.at("notice").get<std::string>();
    if (!notice.empty() && notice != logged_notice)
        log_event("NOTICE", {field("busy", state.at("busy").get<bool>() ? "true" : "false"), field("message", notice.substr(0, 2048).c_str())});
    logged_notice = notice;
    std::map<std::string, std::string> current;
    for (const auto& source : state.at("sources")) {
        const auto item = source.at("item").get<std::string>(), error = source.at("error").get<std::string>();
        if (error.empty()) continue;
        const auto previous = logged_source_errors.find(item);
        if (previous == logged_source_errors.end() || previous->second != error)
            log_event("SOURCE_ERROR", {field("item", item.c_str()), field("error", error.substr(0, 2048).c_str())});
        current.emplace(item, error);
    }
    logged_source_errors = std::move(current);
}

// Storage the folder chooser may show, as ps5upload's volume list does: internal user data, extended
// storage (M.2 or USB formatted by the PS5) and USB drives.
constexpr std::size_t folder_list_limit = 2000;

struct Volume {
    std::filesystem::path path;
    std::string label;
    bool writable;
    unsigned long long free_bytes;
};

bool storage_mount_point(const char* path) {
    if (std::strcmp(path, "/data") == 0) return true;
    if (std::strncmp(path, "/mnt/ext", 8) != 0 && std::strncmp(path, "/mnt/usb", 8) != 0) return false;
    const char* rest = path + 8;
    if (*rest < '0' || *rest > '9') return false;
    if (*++rest >= '0' && *rest <= '9') ++rest;
    return *rest == '\0';
}

std::string volume_label(const std::string& path) {
    if (path == "/data") return "Internal storage";
    return (path.compare(0, 8, "/mnt/ext") == 0 ? "Extended storage " : "USB drive ") + path.substr(8);
}

std::vector<Volume> available_volumes() {
    std::vector<Volume> volumes;
#ifdef PS5_PAYLOAD
    struct statfs* mounts = nullptr;
    // MNT_NOWAIT reads the cached table; a fresh statfs can hang on a faulty drive.
    const int count = getmntinfo(&mounts, MNT_NOWAIT);
    for (int index = 0; index < count && mounts; ++index) {
        const struct statfs& mount = mounts[index];
        if (!storage_mount_point(mount.f_mntonname)) continue;
        // Empty USB and extended slots exist as folders; only a mounted real device counts.
        const bool internal = std::strcmp(mount.f_mntonname, "/data") == 0;
        if (!internal && (std::strncmp(mount.f_mntfromname, "/dev/", 5) != 0 || mount.f_blocks == 0)) continue;
        const auto available = static_cast<long long>(mount.f_bavail) > 0 ? static_cast<unsigned long long>(mount.f_bavail) : 0ULL;
        volumes.push_back({mount.f_mntonname, volume_label(mount.f_mntonname), (mount.f_flags & MNT_RDONLY) == 0,
            available * static_cast<unsigned long long>(mount.f_bsize)});
    }
#else
    for (const char* candidate : {"/data", "/mnt/ext0", "/mnt/ext1", "/mnt/usb0", "/mnt/usb1"}) {
        std::error_code error;
        if (!std::filesystem::is_directory(candidate, error)) continue;
        const auto space = std::filesystem::space(candidate, error);
        volumes.push_back({candidate, volume_label(candidate), access(candidate, W_OK) == 0, error ? 0ULL : space.available});
    }
#endif
    return volumes;
}

bool within(const std::filesystem::path& path, const std::filesystem::path& root) {
    const auto mismatch = std::mismatch(root.begin(), root.end(), path.begin(), path.end());
    return mismatch.first == root.end();
}

bool has_dot_component(const std::filesystem::path& path) {
    for (const auto& part : path) if (part == "." || part == "..") return true;
    return false;
}

// "." and ".." are refused before resolving, then the resolved path (links followed) must lie on a volume.
std::filesystem::path resolve_folder(const std::string& text, std::filesystem::path* root_out = nullptr) {
    if (text.empty() || text.size() > 4096 || text.front() != '/') throw std::runtime_error("Choose a folder on the console");
    const auto requested = std::filesystem::u8path(text);
    if (has_dot_component(requested)) throw std::runtime_error("Folder paths cannot contain . or ..");
    std::error_code error;
    const auto resolved = std::filesystem::canonical(requested, error);
    if (error || !std::filesystem::is_directory(resolved, error)) throw std::runtime_error("That folder does not exist");
    for (const auto& volume : available_volumes()) {
        const auto root = std::filesystem::canonical(volume.path, error);
        if (!error && within(resolved, root)) {
            if (root_out) *root_out = root;
            return resolved;
        }
    }
    throw std::runtime_error("Only internal storage (/data), extended storage and USB drives can be used");
}

Json folder_listing(const std::string& text) {
    Json folders = Json::array();
    if (text.empty()) {
        for (const auto& volume : available_volumes())
            folders.push_back({{"name", volume.label + " (" + volume.path.u8string() + ")"}, {"path", volume.path.u8string()},
                {"free", volume.free_bytes}, {"writable", volume.writable}});
        return {{"path", ""}, {"parent", ""}, {"writable", false}, {"folders", folders}, {"truncated", false}};
    }
    std::filesystem::path root;
    const auto folder = resolve_folder(text, &root);
    std::vector<std::pair<std::string, std::string>> entries;
    bool truncated = false;
    std::error_code error;
    for (std::filesystem::directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        std::error_code type_error;
        if (!it->is_directory(type_error) || type_error) continue;
        if (it->is_symlink(type_error)) {
            // Only list links whose target stays on the same storage.
            const auto target = std::filesystem::canonical(it->path(), type_error);
            if (type_error || !within(target, root)) continue;
        }
        if (entries.size() >= folder_list_limit) { truncated = true; break; }
        entries.emplace_back(it->path().filename().u8string(), it->path().u8string());
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return std::lexicographical_compare(left.first.begin(), left.first.end(), right.first.begin(), right.first.end(),
            [](unsigned char a, unsigned char b) { return std::tolower(a) < std::tolower(b); });
    });
    for (const auto& entry : entries) folders.push_back({{"name", entry.first}, {"path", entry.second}});
    const std::string parent = folder == root ? std::string() : folder.parent_path().u8string();
    return {{"path", folder.u8string()}, {"parent", parent}, {"writable", access(folder.c_str(), W_OK) == 0},
        {"folders", folders}, {"truncated", truncated}};
}

Json create_folder(const Json& value) {
    const auto parent = resolve_folder(value.at("path").get<std::string>());
    const auto name = value.at("name").get<std::string>();
    if (name.empty() || name.size() > 255 || name == "." || name == ".." ||
        std::any_of(name.begin(), name.end(), [](unsigned char character) { return character < 32 || character == '/'; }))
        throw std::runtime_error("Use a folder name without slashes");
    const auto created = parent / std::filesystem::u8path(name);
    std::error_code error;
    if (!std::filesystem::create_directory(created, error)) {
        throw std::runtime_error(error ? "Cannot create the folder: " + error.message() : std::string("That folder already exists"));
    }
    log_event("FOLDER_CREATED", {field("path", created.u8string().c_str())});
    return {{"path", created.u8string()}};
}

void log_transfer(const TransferReport& report) {
    const long long kib_per_second = report.total_us > 0 ?
        static_cast<long long>(report.bytes * 1000000ULL / 1024ULL / static_cast<unsigned long long>(report.total_us)) : 0;
    log_event("DOWNLOAD_TRANSFER", {field("name", report.name.c_str()), field("host", report.host.c_str()),
        field("address", report.address.c_str()), field("bytes", static_cast<long long>(report.bytes)),
        field("total_ms", report.total_us / 1000), field("connect_ms", report.connect_us / 1000),
        field("tls_ms", report.tls_us / 1000), field("kib_per_s", kib_per_second),
        field("http_version", static_cast<long long>(report.http_version)), field("connections", report.connections),
        field("rcvbuf_default", report.receive_buffer_default), field("rcvbuf", report.receive_buffer)});
}

using ApiHandler = std::function<void(const httplib::Request&, httplib::Response&)>;

// Checks Host, Origin, cross-site and (for /api/*) the session token, then runs the handler under the lock.
httplib::Server::Handler guarded(bool needs_session, ApiHandler handler) {
    return [needs_session, handler](const httplib::Request& request, httplib::Response& response) {
        if (const char* reason = denial(request, needs_session)) {
            if (denials_logged.fetch_add(1) < denial_log_limit)
                log_event("API_ACCESS_DENIED", {field("reason", reason), field("path", request.path.c_str()),
                    field("peer", request.remote_addr.c_str()), field("host", request.get_header_value("Host").c_str())});
            response.status = 403;
            set_json(response, "{\"error\":\"Access denied\"}");
            return;
        }
        std::lock_guard<std::mutex> lock(application_mutex);
        if (!application) {
            response.status = 503;
            set_json(response, "{\"error\":\"ArchiveFetch is not running; see the diagnostic log\"}");
            return;
        }
        handler(request, response);
    };
}

}

bool api_accept_peer(const httplib::Request& request, httplib::Response& response) {
    if (local_peer(request.remote_addr)) return true;
    const char* reason = !external_access.load() ? "external" : !password_set() ? "no-password" : nullptr;
    if (!reason) {
        if (signed_in(request) || !paired_device(request).empty()) return true;
        // Before signing in, an outside device only gets the page itself, /session and the sign-in call.
        const auto& path = request.path;
        if (path == "/api/login" || path == "/session" ||
            (request.method == "GET" && path.rfind("/api/", 0) != 0 && path != "/status" && path != "/diag")) return true;
        response.status = 401;
        set_json(response, "{\"error\":\"Sign in required\",\"login\":true}");
        return false;
    }
    if (denials_logged.fetch_add(1) < denial_log_limit)
        log_event("API_ACCESS_DENIED", {field("reason", reason), field("path", request.path.c_str()),
            field("peer", request.remote_addr.c_str())});
    response.status = 403;
    response.set_content(std::strcmp(reason, "external") == 0
        ? "ArchiveFetch only accepts devices on the console's local network. "
          "Turn on External connections in Settings on the console to allow others.\n"
        : "ArchiveFetch needs a password before devices outside the console's network can connect. "
          "Set one in Settings > Network on the console.\n", "text/plain; charset=utf-8");
    return false;
}

bool api_init(const char* data_root) {
    log_event("APP_INIT_BEGIN", {field("data_root", data_root)});
    try {
        const auto root = std::filesystem::u8path(data_root);
        std::filesystem::create_directories(root);
        // Relative defaults such as the download and export folders resolve under the data root.
        std::filesystem::current_path(root);
        network_settings = root / "network.json";
        grants_path = root / "sign-ins.json";
        load_grants();
        paired_path = root / "paired-devices.json";
        load_paired();
        try {
            if (std::filesystem::exists(network_settings)) {
                const auto saved = read_download_state(network_settings);
                external_access = saved.value("external", false);
                if (saved.contains("password")) {
                    const auto& stored = saved.at("password");
                    password = {stored.at("salt").get<std::string>(), stored.at("hash").get<std::string>(), stored.at("iterations").get<int>()};
                }
            }
        } catch (const std::exception& failure) {
            log_event("NETWORK_SETTINGS_INVALID", {field("error", failure.what())});
        }
        log_event("NETWORK_ACCESS", {field("external", external_access ? "true" : "false"), field("password", password.hash.empty() ? "false" : "true"),
            field("sign_ins", static_cast<long long>(grants.size()))});
        const std::string embedded_ca = install_ca_bundle(data_root);
        auto log = std::make_unique<DiagnosticLog>();
        auto app = std::make_unique<WebApplication>(root);
        app->extract = run_extraction;
        // curl's compiled-in default CA file does not exist on the console; a path set in Settings still wins.
        const char* ca_source = "settings";
        if (app->config.ca_bundle.empty()) {
            app->config.ca_bundle = embedded_ca;
            ca_source = embedded_ca.empty() ? "none" : "embedded";
        }
        const auto state = app->model.snapshot();
        set_transfer_observer(log_transfer);
        log_event("APP_INIT_END", {field("sources", static_cast<long long>(state.sources.size())),
            field("files", static_cast<long long>(state.catalog.files.size())),
            field("download_dir", app->config.download_dir.u8string().c_str()),
            field("output_dir", app->config.output_dir.u8string().c_str()),
            field("ca_bundle", app->config.ca_bundle.c_str()), field("ca_source", ca_source),
            field("load_error", app->error.c_str())});
        std::lock_guard<std::mutex> lock(application_mutex);
        session_token = random_token();
        diagnostics = std::move(log);
        application = std::move(app);
        return true;
    } catch (const std::exception& failure) {
        log_event("APP_INIT_FAILED", {field("error", failure.what())});
        return false;
    }
}

void api_register_routes(httplib::Server& http) {
    http.Get("/session", guarded(false, [](const httplib::Request& request, httplib::Response& response) {
        const auto agent = request.get_header_value("User-Agent").substr(0, 300);
        if (logged_agents.size() < 16 && logged_agents.insert(agent).second)
            log_event("CLIENT_BROWSER", {field("peer", request.remote_addr.c_str()), field("user_agent", agent.c_str())});
        const bool local = local_peer(request.remote_addr);
        const auto device = paired_device(request);
        if (!local && !signed_in(request) && device.empty()) {
            response.status = 401;
            set_json(response, Json({{"error", "Sign in required"}, {"login", true}, {"version", version}, {"pairing", true}}).dump());
            return;
        }
        set_json(response, Json({{"token", session_token}, {"network", true}, {"folderBrowser", true}, {"version", version},
            {"remoteAccess", true}, {"logFile", true}, {"signedIn", !local}, {"pairing", true}, {"paired", !device.empty()}}).dump());
    }));
    // An app pairs once (from the local network, or after signing in from outside) and then needs no password.
    http.Post("/api/pair", guarded(true, [](const httplib::Request& request, httplib::Response& response) {
        try {
            if (request.get_header_value("Content-Type") != "application/json") throw std::runtime_error("Expected JSON request");
            const auto name = Json::parse(request.body).at("name").get<std::string>();
            if (name.empty() || name.size() > 64 || std::any_of(name.begin(), name.end(), [](unsigned char character) { return character < 32; }))
                throw std::runtime_error("Name the device with up to 64 characters");
            const auto id = random_token().substr(0, 16), key = random_token();
            {
                std::lock_guard<std::mutex> lock(auth_mutex);
                if (paired.size() >= paired_limit) throw std::runtime_error("32 devices are paired; remove one in Settings > Network first");
                paired[id] = {name, grant_digest(key), now_seconds(), now_seconds()};
                save_paired();
            }
            log_event("DEVICE_PAIRED", {field("id", id.c_str()), field("name", name.c_str()), field("peer", request.remote_addr.c_str())});
            set_json(response, Json({{"id", id}, {"key", key}}).dump());
        } catch (const std::exception& failure) {
            response.status = 400;
            set_json(response, Json({{"error", failure.what()}}).dump());
        }
    }));
    http.Get("/api/paired-devices", guarded(true, [](const httplib::Request& request, httplib::Response& response) {
        const auto current = paired_device(request);
        Json devices = Json::array();
        {
            std::lock_guard<std::mutex> lock(auth_mutex);
            for (const auto& device : paired)
                devices.push_back({{"id", device.first}, {"name", device.second.name}, {"created", device.second.created},
                    {"lastSeen", device.second.last_seen}, {"current", device.first == current}});
        }
        set_json(response, Json({{"devices", devices}}).dump());
    }));
    http.Post("/api/paired-devices/remove", guarded(true, [](const httplib::Request& request, httplib::Response& response) {
        try {
            if (request.get_header_value("Content-Type") != "application/json") throw std::runtime_error("Expected JSON request");
            const auto id = Json::parse(request.body).at("id").get<std::string>();
            std::lock_guard<std::mutex> lock(auth_mutex);
            if (!paired.erase(id)) throw std::runtime_error("That device is no longer paired");
            save_paired();
            log_event("DEVICE_UNPAIRED", {field("id", id.substr(0, 16).c_str()), field("peer", request.remote_addr.c_str())});
            set_json(response, "{\"ok\":true}");
        } catch (const std::exception& failure) {
            response.status = 400;
            set_json(response, Json({{"error", failure.what()}}).dump());
        }
    }));
    http.Post("/api/login", guarded(false, [](const httplib::Request& request, httplib::Response& response) {
        const auto& peer = request.remote_addr;
        try {
            if (request.get_header_value("Content-Type") != "application/json") throw std::runtime_error("Expected JSON request");
            if (local_peer(peer)) { set_json(response, "{\"ok\":true}"); return; }
            auto body = Json::parse(request.body);
            auto attempt = body.at("password").get<std::string>();
            Password stored;
            {
                std::lock_guard<std::mutex> lock(auth_mutex);
                const auto now = now_seconds();
                const auto& lock_state = failures[peer];
                const long long until = std::max(global_lock_until, lock_state.locked_until);
                if (until > now) {
                    response.status = 429;
                    set_json(response, Json({{"error", "Too many wrong passwords. Try again in " + std::to_string((until - now + 59) / 60) + " minutes."}}).dump());
                    return;
                }
                stored = password;
            }
            const bool correct = !stored.hash.empty() && attempt.size() <= password_max &&
                CRYPTO_memcmp(derive_password(attempt, stored.salt, stored.iterations).data(), stored.hash.data(), stored.hash.size()) == 0;
            OPENSSL_cleanse(&attempt[0], attempt.size());
            std::lock_guard<std::mutex> lock(auth_mutex);
            const auto now = now_seconds();
            if (correct) {
                failures.erase(peer);
                while (grants.size() >= grant_limit) {
                    grants.erase(std::min_element(grants.begin(), grants.end(), [](const auto& left, const auto& right) { return left.second < right.second; }));
                }
                const auto token = random_token();
                grants[grant_digest(token)] = now + grant_seconds;
                save_grants();
                response.set_header("Set-Cookie", std::string(auth_cookie) + "=" + token + "; Path=/; Max-Age=" + std::to_string(grant_seconds) + "; HttpOnly; SameSite=Strict");
                log_event("LOGIN_OK", {field("peer", peer.c_str())});
                set_json(response, "{\"ok\":true}");
                return;
            }
            if (failures.size() > 1024) failures.clear();
            auto& failed = failures[peer];
            if (++failed.count >= peer_failures) {
                failed = {0, now + peer_lock_seconds};
                log_event("LOGIN_LOCKED", {field("peer", peer.c_str()), field("minutes", peer_lock_seconds / 60)});
            }
            recent_failures.push_back(now);
            while (!recent_failures.empty() && recent_failures.front() <= now - 3600) recent_failures.pop_front();
            if (static_cast<int>(recent_failures.size()) >= global_failures && global_lock_until <= now) {
                global_lock_until = now + global_lock_seconds;
                recent_failures.clear();
                log_event("LOGIN_PAUSED", {field("minutes", global_lock_seconds / 60)});
                notify("many wrong passwords from outside your network; sign-in paused for an hour");
            }
            log_event("LOGIN_FAILED", {field("peer", peer.c_str())});
            response.status = 401;
            set_json(response, "{\"error\":\"Wrong password\"}");
        } catch (const std::exception& failure) {
            response.status = 400;
            set_json(response, Json({{"error", failure.what()}}).dump());
        }
    }));
    http.Post("/api/logout", guarded(true, [](const httplib::Request& request, httplib::Response& response) {
        const auto presented = presented_grant(request);
        {
            std::lock_guard<std::mutex> lock(auth_mutex);
            if (!presented.empty() && presented.size() <= 128) grants.erase(grant_digest(presented));
            save_grants();
        }
        response.set_header("Set-Cookie", std::string(auth_cookie) + "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
        set_json(response, "{\"ok\":true}");
    }));
    http.Post("/api/password", guarded(true, [](const httplib::Request& request, httplib::Response& response) {
        try {
            if (!local_peer(request.remote_addr)) throw std::runtime_error("Change the password from the console or a device on its local network");
            if (request.get_header_value("Content-Type") != "application/json") throw std::runtime_error("Expected JSON request");
            auto text = Json::parse(request.body).at("password").get<std::string>();
            if (!text.empty() && (text.size() < password_min || text.size() > password_max))
                throw std::runtime_error("Use a password of 8 to 128 characters");
            Password next;
            if (!text.empty()) {
                unsigned char salt[16];
                if (RAND_bytes(salt, sizeof(salt)) != 1) throw std::runtime_error("Cannot create a password salt");
                next = {hex(salt, sizeof(salt)), {}, password_iterations};
                next.hash = derive_password(text, next.salt, next.iterations);
                OPENSSL_cleanse(&text[0], text.size());
            }
            {
                std::lock_guard<std::mutex> lock(auth_mutex);
                password = next;
                // Every outside device has to sign in again with the new password.
                grants.clear();
                save_grants();
            }
            save_network_settings();
            log_event(next.hash.empty() ? "PASSWORD_REMOVED" : "PASSWORD_SET", {field("peer", request.remote_addr.c_str())});
            set_json(response, Json({{"external", external_access.load()}, {"local", true}, {"passwordSet", !next.hash.empty()}, {"signedIn", false}}).dump());
        } catch (const std::exception& failure) {
            response.status = 400;
            set_json(response, Json({{"error", failure.what()}}).dump());
        }
    }));
    http.Get("/api/remote-access", guarded(true, [](const httplib::Request& request, httplib::Response& response) {
        const bool local = local_peer(request.remote_addr);
        set_json(response, Json({{"external", external_access.load()}, {"local", local}, {"passwordSet", password_set()},
            {"signedIn", !local && signed_in(request)}}).dump());
    }));
    http.Post("/api/remote-access", guarded(true, [](const httplib::Request& request, httplib::Response& response) {
        try {
            if (!local_peer(request.remote_addr)) throw std::runtime_error("Change this from a device on the console's local network");
            if (request.get_header_value("Content-Type") != "application/json") throw std::runtime_error("Expected JSON request");
            const bool external = Json::parse(request.body).at("external").get<bool>();
            if (external && !password_set()) throw std::runtime_error("Set a password first");
            external_access = external;
            save_network_settings();
            log_event("NETWORK_ACCESS_CHANGED", {field("external", external ? "true" : "false"), field("peer", request.remote_addr.c_str())});
            set_json(response, Json({{"external", external}, {"local", true}, {"passwordSet", password_set()}, {"signedIn", false}}).dump());
        } catch (const std::exception& failure) {
            response.status = 400;
            set_json(response, Json({{"error", failure.what()}}).dump());
        }
    }));
    http.Get("/api/log", guarded(true, [](const httplib::Request&, httplib::Response& response) {
        set_json(response, Json({{"bytes", log_size(log_path)}, {"previousBytes", log_size(log_previous_path)}, {"limit", log_rotate_bytes}}).dump());
    }));
    http.Get("/api/log/entries", guarded(true, [](const httplib::Request& request, httplib::Response& response) {
        const int limit = std::clamp(std::atoi(request.get_param_value("limit").c_str()), 1, 2000);
        std::ifstream input(log_path, std::ios::binary | std::ios::ate);
        const long long size = input ? static_cast<long long>(input.tellg()) : 0;
        // At most the last 2 MiB; a partial first line is skipped by the parse check.
        const long long start = std::max(0LL, size - 2LL * 1024 * 1024);
        std::string text(static_cast<std::size_t>(size - start), '\0');
        if (input) { input.seekg(start); input.read(&text[0], static_cast<std::streamsize>(text.size())); }
        std::vector<Json> entries;
        std::size_t end = text.size();
        while (end > 0 && static_cast<int>(entries.size()) < limit) {
            const auto newline = text.rfind('\n', end - 1);
            const auto begin = newline == std::string::npos ? 0 : newline + 1;
            if (end > begin) {
                auto entry = Json::parse(text.begin() + static_cast<std::ptrdiff_t>(begin), text.begin() + static_cast<std::ptrdiff_t>(end), nullptr, false);
                if (entry.is_object() && entry.contains("event"))
                    entries.push_back({{"event", entry.value("event", "")}, {"version", entry.value("version", "")},
                        {"time", entry.value("local_timestamp", "")}, {"details", entry.value("details", Json::object())}});
            }
            if (newline == std::string::npos) break;
            end = newline;
        }
        set_json(response, Json({{"entries", entries}, {"bytes", size}}).dump());
    }));
    http.Post("/api/log/clear", guarded(true, [](const httplib::Request& request, httplib::Response& response) {
        if (!log_clear()) {
            response.status = 500;
            set_json(response, "{\"error\":\"Cannot clear the log\"}");
            return;
        }
        log_event("LOG_CLEARED", {field("peer", request.remote_addr.c_str())});
        set_json(response, Json({{"bytes", log_size(log_path)}, {"previousBytes", log_size(log_previous_path)}, {"limit", log_rotate_bytes}}).dump());
    }));
    http.Get("/api/network", guarded(true, [](const httplib::Request& request, httplib::Response& response) {
        set_json(response, network_urls(request.local_addr).dump());
    }));
    http.Get("/api/folders", guarded(true, [](const httplib::Request& request, httplib::Response& response) {
        try {
            set_json(response, folder_listing(request.get_param_value("path")).dump());
        } catch (const std::exception& failure) {
            response.status = 400;
            set_json(response, Json({{"error", failure.what()}}).dump());
        }
    }));
    http.Post("/api/folders", guarded(true, [](const httplib::Request& request, httplib::Response& response) {
        try {
            if (request.get_header_value("Content-Type") != "application/json") throw std::runtime_error("Expected JSON request");
            set_json(response, create_folder(Json::parse(request.body)).dump());
        } catch (const std::exception& failure) {
            response.status = 400;
            set_json(response, Json({{"error", failure.what()}}).dump());
        }
    }));
    http.Get("/api/state", guarded(true, [](const httplib::Request&, httplib::Response& response) {
        const auto state = application->snapshot();
        log_queue_changes(state);
        log_scan_changes(state);
        set_json(response, state.dump());
    }));
    http.Get("/api/library", guarded(true, [](const httplib::Request&, httplib::Response& response) {
        set_json(response, application->library().dump());
    }));
    http.Get("/api/diagnostics", guarded(true, [](const httplib::Request&, httplib::Response& response) {
        set_json(response, Json({{"log", diagnostics->text()}}).dump());
    }));
    http.Post("/api/diagnostics/clear", guarded(true, [](const httplib::Request&, httplib::Response& response) {
        diagnostics->clear();
        set_json(response, "{}");
    }));
    http.Post("/api/command", guarded(true, [](const httplib::Request& request, httplib::Response& response) {
        std::string action;
        try {
            if (request.get_header_value("Content-Type") != "application/json") throw std::runtime_error("Expected JSON request");
            const auto body = Json::parse(request.body);
            action = body.value("action", "");
            const auto result = application->command(body);
            if (action == "deleteDownload")
                log_event("DOWNLOAD_DELETED", {field("path", body.at("path").get<std::string>().c_str()),
                    field("remove_entry", body.value("removeEntry", false) ? "true" : "false")});
            else if (action == "cleanLibrary" && !body.value("preview", false))
                log_event("LIBRARY_CLEANED", {field("mode", body.value("mode", "").c_str()), field("scope", body.contains("indices") ? "selected" : "all"),
                    field("removed", static_cast<long long>(result.value("removed", 0))), field("deleted", static_cast<long long>(result.value("deleted", 0)))});
            else if (action == "cleanDownloads")
                log_event("DOWNLOADS_CLEANED", {field("removed", std::to_string(result.at("removed").get<std::size_t>()).c_str())});
            set_json(response, result.dump());
        } catch (const std::exception& failure) {
            // The action name and message only; the request body can hold S3 keys.
            log_event("COMMAND_FAILED", {field("action", action.substr(0, 64).c_str()), field("error", failure.what())});
            response.status = 400;
            set_json(response, Json({{"error", failure.what()}}).dump());
        }
    }));
}

std::string api_handoff_keys() {
    std::lock_guard<std::mutex> lock(application_mutex);
    // Saved keys are loaded from disk by the successor, so only unsaved ones travel.
    if (!application || application->remember || application->config.s3.access_key.empty() || application->config.s3.secret_key.empty())
        return "{}";
    log_event("KEYS_HANDED_OVER");
    return Json({{"access", application->config.s3.access_key}, {"secret", application->config.s3.secret_key}}).dump();
}

void api_adopt_keys(std::string& access_key, std::string& secret_key) {
    const char* outcome = "adopted";
    {
        std::lock_guard<std::mutex> lock(application_mutex);
        if (!application) outcome = "app-not-running";
        else if (!application->config.s3.access_key.empty()) outcome = "saved-keys-loaded";
        else {
            S3Credentials keys{access_key, secret_key};
            try {
                keys.authorization();
                application->config.s3 = keys;
            } catch (const std::exception&) { outcome = "invalid"; }
            OPENSSL_cleanse(&keys.access_key[0], keys.access_key.size());
            OPENSSL_cleanse(&keys.secret_key[0], keys.secret_key.size());
        }
    }
    OPENSSL_cleanse(&access_key[0], access_key.size());
    OPENSSL_cleanse(&secret_key[0], secret_key.size());
    access_key.clear(); secret_key.clear();
    log_event("KEYS_FROM_PREVIOUS", {field("outcome", outcome)});
}

void api_shutdown() {
    std::unique_ptr<WebApplication> stopping;
    {
        std::lock_guard<std::mutex> lock(application_mutex);
        stopping = std::move(application);
    }
    if (!stopping) return;
    log_event("APP_SHUTDOWN_BEGIN");
    // The destructor cancels running work and saves the library.
    stopping.reset();
    log_event("APP_SHUTDOWN_END");
}

}
