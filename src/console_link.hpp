#pragma once
// Saved PS5 consoles and the relay that lets the Windows and Android apps control one. The page never
// talks to a console itself: the app backend sends each request over plain HTTP to a saved address only,
// keeps the console's session token, and holds the sign-in cookie for consoles outside the local network.
#include "web_application.hpp"
#include "version.hpp"
#include <curl/curl.h>
#include <openssl/evp.h>
#include <condition_variable>
#include <functional>
#include <future>
#include <thread>
#ifdef _WIN32
#include <dpapi.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

namespace {

constexpr std::size_t console_limit = 32;
constexpr std::size_t relay_response_limit = 64 * 1024 * 1024;
constexpr const char* console_cookie = "ArchiveFetchAuth";

struct ConsoleEntry {
    std::string id, name, address;
    int port = 8890;
    // The pairing key as protected by the platform, and whether the console has since removed this device.
    std::string sealed;
    bool unpaired = false;
    // The console's ELF loader, used only by Install/Update.
    int loader = 9021;
};

struct ConsoleReply {
    int status = 0;
    std::string body;
};

// Pairing keys are only kept through these; without them the app never pairs.
struct ConsoleSecrets {
    std::function<std::string(const std::string&)> protect, unprotect;
    std::string device_name;
};

std::string base64_text(const std::string& bytes) {
    std::string text(4 * ((bytes.size() + 2) / 3), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&text[0]), reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()));
    return text;
}

std::string base64_bytes(const std::string& text) {
    if (text.empty() || text.size() % 4) throw std::runtime_error("Invalid saved console key");
    std::string bytes(text.size() / 4 * 3, '\0');
    const int size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(&bytes[0]), reinterpret_cast<const unsigned char*>(text.data()), static_cast<int>(text.size()));
    if (size < 0) throw std::runtime_error("Invalid saved console key");
    const auto padding = static_cast<std::size_t>(std::count(text.end() - 2, text.end(), '='));
    bytes.resize(static_cast<std::size_t>(size) - padding);
    return bytes;
}

#ifdef _WIN32
std::string dpapi_text(const std::string& text, bool protect) {
    DATA_BLOB source{static_cast<DWORD>(text.size()), reinterpret_cast<BYTE*>(const_cast<char*>(text.data()))}, result{};
    const BOOL done = protect
        ? CryptProtectData(&source, L"ArchiveFetch console key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &result)
        : CryptUnprotectData(&source, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &result);
    if (!done) throw std::runtime_error(protect ? "Windows could not protect the console key" : "The saved console key cannot be unlocked");
    std::string output(reinterpret_cast<const char*>(result.pbData), result.cbData);
    SecureZeroMemory(result.pbData, result.cbData);
    LocalFree(result.pbData);
    return output;
}
#endif

// Android supplies its own (Keystore) in the bridge; other desktops have no secret store and do not pair.
ConsoleSecrets platform_console_secrets() {
#ifdef _WIN32
    char computer[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = sizeof(computer);
    const std::string name = GetComputerNameA(computer, &size) ? std::string(computer, size) : std::string("PC");
    return {[](const std::string& text) { return base64_text(dpapi_text(text, true)); },
        [](const std::string& text) { return dpapi_text(base64_bytes(text), false); }, "Windows: " + name};
#else
    return {};
#endif
}

class ConsoleLink {
public:
    explicit ConsoleLink(std::filesystem::path file, ConsoleSecrets secrets = {}) : file_(std::move(file)), secrets_(std::move(secrets)) {
        try {
            if (std::filesystem::exists(file_)) {
                std::ifstream input(file_, std::ios::binary);
                const auto document = Json::parse(input);
                for (const auto& saved : document.at("consoles")) {
                    ConsoleEntry entry{saved.at("id"), saved.at("name"), saved.at("address"), saved.value("port", 8890),
                        saved.value("pairing", ""), saved.value("unpaired", false), saved.value("loader", 9021)};
                    validate(entry);
                    if (consoles_.size() >= console_limit) continue;
                    if (!entry.sealed.empty()) {
                        try {
                            if (!secrets_.unprotect) throw std::runtime_error("no secret store");
                            const auto key = secrets_.unprotect(entry.sealed);
                            if (key.size() != 81 || key[16] != ':') throw std::runtime_error("invalid key");
                            keys_[entry.id] = key;
                        } catch (const std::exception& failure) {
                            std::cerr << "[consoles] Pairing for " << entry.name << " cannot be used: " << failure.what() << '\n';
                            entry.sealed.clear();
                        }
                    }
                    consoles_.push_back(entry);
                }
            }
        } catch (const std::exception& failure) {
            std::cerr << "[consoles] Saved consoles are invalid and were ignored: " << failure.what() << '\n';
            consoles_.clear(); keys_.clear();
        }
    }
    ~ConsoleLink() {
        { std::lock_guard<std::mutex> guard(mutex_); stopping_ = true; }
        wake_.notify_all();
        if (prober_.joinable()) prober_.join();
        if (scanner_.joinable()) scanner_.join();
        if (installer_.joinable()) installer_.join();
    }

    // Tests point the scan at loopback; empty means the local network's /24.
    std::vector<std::string> scan_hosts;
    int scan_port = 8890;
    // The bundled PS5 payload (ArchiveFetch.elf with ArchiveFetch.version) offered by Install/Update.
    std::filesystem::path payload_dir;
    int install_wait_seconds = 120;

    bool payload_available() const {
        std::ifstream version(payload_dir / "ArchiveFetch.version");
        std::string text;
        std::getline(version, text);
        std::error_code error;
        return text == ARCHIVEFETCH_VERSION && std::filesystem::is_regular_file(payload_dir / "ArchiveFetch.elf", error);
    }

    // Statuses come from a background check every 15 s while the page keeps asking for them.
    Json list(bool refresh = false) {
        std::unique_lock<std::mutex> guard(mutex_);
        last_listed_ = std::chrono::steady_clock::now();
        if (refresh) refresh_ = true;
        if (!prober_running_) {
            if (prober_.joinable()) { guard.unlock(); prober_.join(); guard.lock(); }
            if (!prober_running_ && !stopping_) { prober_running_ = true; refresh_ = true; prober_ = std::thread([this] { probe_loop(); }); }
        }
        wake_.notify_all();
        Json result = Json::array();
        for (const auto& entry : consoles_) {
            Json value = {{"id", entry.id}, {"name", entry.name}, {"address", entry.address}, {"port", entry.port}, {"status", "checking"},
                {"paired", keys_.count(entry.id) > 0}, {"loader", entry.loader}};
            const auto found = status_.find(entry.id);
            if (found != status_.end()) for (const auto& field : found->second.items()) value[field.key()] = field.value();
            const auto install = installs_.find(entry.id);
            if (install != installs_.end()) value["install"] = install->second;
            result.push_back(value);
        }
        return {{"consoles", result}, {"appVersion", ARCHIVEFETCH_VERSION}, {"scan", scan_}, {"payload", payload_available()}};
    }

    Json command(const Json& value) {
        const auto action = value.at("action").get<std::string>();
        if (action == "scan") { start_scan(); return list(); }
        if (action == "install") { start_install(value.at("id").get<std::string>()); return list(); }
        {
            std::lock_guard<std::mutex> guard(mutex_);
            auto next = consoles_;
            if (action == "add") {
                if (next.size() >= console_limit) throw std::runtime_error("Save at most 32 consoles");
                ConsoleEntry entry{random_token().substr(0, 16), value.at("name"), value.at("address"), value.value("port", 8890)};
                entry.loader = value.value("loader", 9021);
                validate(entry);
                next.push_back(entry);
            } else if (action == "edit" || action == "remove") {
                const auto id = value.at("id").get<std::string>();
                const auto found = std::find_if(next.begin(), next.end(), [&](const ConsoleEntry& entry) { return entry.id == id; });
                if (found == next.end()) throw std::runtime_error("That console is no longer saved");
                if (action == "remove") { next.erase(found); keys_.erase(id); }
                else {
                    ConsoleEntry entry{id, value.at("name"), value.at("address"), value.value("port", 8890)};
                    entry.loader = value.value("loader", found->loader);
                    validate(entry);
                    // The key only goes back to the same address; pointing the entry elsewhere pairs again.
                    if (entry.address == found->address && entry.port == found->port) { entry.sealed = found->sealed; entry.unpaired = found->unpaired; }
                    else keys_.erase(id);
                    *found = entry;
                }
                sessions_.erase(id); status_.erase(id);
            } else throw std::runtime_error("Unknown console action");
            save(next);
            consoles_ = std::move(next);
        }
        return list(true);
    }

    // Path is the console route, such as /api/state; only /session and /api/* are relayed.
    ConsoleReply relay(const std::string& id, const std::string& method, const std::string& path, const std::string& body) {
        if (method != "GET" && method != "POST") throw std::runtime_error("Unsupported console request");
        if (path.size() > 4096 || (path != "/session" && path.rfind("/api/", 0) != 0) || path.find("..") != std::string::npos ||
            std::any_of(path.begin(), path.end(), [](unsigned char character) { return character <= 32 || character >= 127 || character == '#'; }))
            throw std::runtime_error("That console request is not allowed");
        const auto entry = find(id);
        if (path == "/session") return session(entry);
        if (path == "/api/login") {
            auto reply = send(entry, "POST", path, body, false, 30);
            if (reply.status == 200) {
                // Signing in with the password allows pairing again after the console removed this device.
                set_unpaired(id, false);
                session(find(id), true);
            }
            return reply;
        }
        if (token(id).empty()) {
            auto opened = session(entry);
            if (opened.status != 200) return opened;
        }
        auto reply = send(entry, method, path, body, true, 60);
        // The console restarted and issued a new token: renew it once.
        if (reply.status == 403) {
            auto opened = session(entry);
            if (opened.status != 200) return opened;
            reply = send(entry, method, path, body, true, 60);
        }
        // A 403 reaching the page would look like a stale app session, so it is reported as a console error.
        if (reply.status == 403) reply = {502, Json({{"error", "The console refused the request"}}).dump()};
        return reply;
    }

private:
    struct Session { std::string token, cookie; };
    struct Response { long status = 0; std::string body, cookie; bool cookie_seen = false; };
    struct Upload { std::ifstream input; std::function<void(std::uint64_t)> progress; std::uint64_t sent; };

    static size_t read_upload(char* buffer, size_t size, size_t count, void* data) {
        auto& upload = *static_cast<Upload*>(data);
        upload.input.read(buffer, static_cast<std::streamsize>(size * count));
        const auto read = static_cast<size_t>(upload.input.gcount());
        upload.sent += read;
        if (upload.progress) upload.progress(upload.sent);
        return read;
    }

    static void validate(ConsoleEntry& entry) {
        auto trim = [](std::string text) {
            const auto first = text.find_first_not_of(" \t"), last = text.find_last_not_of(" \t");
            return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
        };
        entry.name = trim(entry.name); entry.address = trim(entry.address);
        if (entry.name.empty() || entry.name.size() > 64 || std::any_of(entry.name.begin(), entry.name.end(), [](unsigned char character) { return character < 32; }))
            throw std::runtime_error("Give the console a name of up to 64 characters");
        const bool address_ok = !entry.address.empty() && entry.address.size() <= 253 && entry.address.front() != '-' && entry.address.front() != '.' &&
            entry.address.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") == std::string::npos;
        if (!address_ok) throw std::runtime_error("Use the console's IP address, such as 192.168.1.20, or a host name");
        if (entry.port < 1 || entry.port > 65535) throw std::runtime_error("Use a port from 1 to 65535");
        if (entry.loader < 1 || entry.loader > 65535) throw std::runtime_error("Use a loader port from 1 to 65535");
        if (entry.id.empty() || entry.id.size() > 32 || entry.id.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("Invalid console id");
    }

    void save(const std::vector<ConsoleEntry>& consoles) {
        Json saved = Json::array();
        for (const auto& entry : consoles) {
            Json value = {{"id", entry.id}, {"name", entry.name}, {"address", entry.address}, {"port", entry.port}};
            if (entry.loader != 9021) value["loader"] = entry.loader;
            if (!entry.sealed.empty()) value["pairing"] = entry.sealed;
            if (entry.unpaired) value["unpaired"] = true;
            saved.push_back(value);
        }
        const auto text = Json({{"version", 1}, {"consoles", saved}}).dump(2);
        auto temporary = file_; temporary += ".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size())); output.close();
        if (!output) throw std::runtime_error("Cannot save the console list");
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot save the console list");
#else
        std::filesystem::rename(temporary, file_);
#endif
    }

    ConsoleEntry find(const std::string& id) {
        std::lock_guard<std::mutex> guard(mutex_);
        for (const auto& entry : consoles_) if (entry.id == id) return entry;
        throw std::runtime_error("That console is no longer saved");
    }

    std::string token(const std::string& id) {
        std::lock_guard<std::mutex> guard(mutex_);
        return sessions_[id].token;
    }

    ConsoleReply session(const ConsoleEntry& entry, bool signed_in_now = false) {
        const bool had_key = has_key(entry.id);
        auto reply = send(entry, "GET", "/session", "", false, 10);
        const auto value = Json::parse(reply.body, nullptr, false);
        const bool pairing = value.is_object() && value.value("pairing", false);
        // A key the console no longer accepts was removed there; the app does not pair again on its own.
        if (had_key && pairing && (reply.status == 401 || (reply.status == 200 && !value.value("paired", false)))) {
            forget_key(entry.id, !signed_in_now);
            std::cerr << "[consoles] " << entry.name << " no longer accepts this device's pairing\n";
        }
        if (reply.status == 200) {
            if (!value.is_object() || !value.contains("token") || !value.at("token").is_string()) return {502, Json({{"error", "That address is not running ArchiveFetch"}}).dump()};
            {
                std::lock_guard<std::mutex> guard(mutex_);
                sessions_[entry.id].token = value.at("token").get<std::string>();
            }
            if (pairing && !value.value("paired", false) && !has_key(entry.id) && (signed_in_now || !find(entry.id).unpaired)) pair(entry);
        }
        return reply;
    }

    bool has_key(const std::string& id) {
        std::lock_guard<std::mutex> guard(mutex_);
        return keys_.count(id) > 0;
    }

    void set_unpaired(const std::string& id, bool unpaired) {
        std::lock_guard<std::mutex> guard(mutex_);
        for (auto& entry : consoles_) if (entry.id == id && entry.unpaired != unpaired) { entry.unpaired = unpaired; save(consoles_); }
    }

    void forget_key(const std::string& id, bool unpaired) {
        std::lock_guard<std::mutex> guard(mutex_);
        keys_.erase(id);
        for (auto& entry : consoles_) if (entry.id == id) { entry.sealed.clear(); entry.unpaired = unpaired; }
        save(consoles_);
    }

    // A failed pairing only means the password is asked again from outside, so it is logged and not reported.
    void pair(const ConsoleEntry& entry) {
        if (!secrets_.protect || secrets_.device_name.empty()) return;
        try {
            const auto reply = send(entry, "POST", "/api/pair", Json({{"name", secrets_.device_name.substr(0, 64)}}).dump(), true, 15);
            const auto value = Json::parse(reply.body, nullptr, false);
            if (reply.status != 200 || !value.is_object()) throw std::runtime_error(value.is_object() ? value.value("error", "HTTP " + std::to_string(reply.status)) : "unreadable reply");
            const auto key = value.at("id").get<std::string>() + ":" + value.at("key").get<std::string>();
            if (key.size() != 81) throw std::runtime_error("unexpected key");
            const auto sealed = secrets_.protect(key);
            std::lock_guard<std::mutex> guard(mutex_);
            for (auto& saved : consoles_) if (saved.id == entry.id) { saved.sealed = sealed; saved.unpaired = false; keys_[entry.id] = key; }
            save(consoles_);
        } catch (const std::exception& failure) {
            std::cerr << "[consoles] Cannot pair with " << entry.name << ": " << failure.what() << '\n';
        }
    }

    // The console only accepts an IP literal in Host, so a host name is resolved here and the request goes to that address.
    static std::string resolve(const std::string& address) {
        addrinfo hints{};
        hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        if (getaddrinfo(address.c_str(), nullptr, &hints, &found) != 0 || !found) throw std::runtime_error("Cannot find " + address);
        char text[INET_ADDRSTRLEN]{};
        const bool converted = inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr, text, sizeof(text)) != nullptr;
        freeaddrinfo(found);
        if (!converted) throw std::runtime_error("Cannot find " + address);
        return text;
    }

    static std::size_t write_body(char* data, std::size_t size, std::size_t count, void* user) {
        auto* response = static_cast<Response*>(user);
        if (response->body.size() + size * count > relay_response_limit) return 0;
        response->body.append(data, size * count);
        return size * count;
    }

    static std::size_t read_header(char* data, std::size_t size, std::size_t count, void* user) {
        auto* response = static_cast<Response*>(user);
        std::string line(data, size * count);
        const std::string name = "set-cookie:";
        if (line.size() > name.size() && std::equal(name.begin(), name.end(), line.begin(), [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); })) {
            auto value = line.substr(name.size());
            value.erase(0, value.find_first_not_of(' '));
            const std::string prefix = std::string(console_cookie) + "=";
            if (value.compare(0, prefix.size(), prefix) == 0) {
                const auto end = value.find_first_of(";\r\n", prefix.size());
                response->cookie = value.substr(prefix.size(), end == std::string::npos ? std::string::npos : end - prefix.size());
                response->cookie_seen = true;
            }
        }
        return size * count;
    }

    ConsoleReply send(const ConsoleEntry& entry, const std::string& method, const std::string& path, const std::string& body, bool with_token, long timeout,
        const std::filesystem::path* upload = nullptr, const std::function<void(std::uint64_t)>& progress = {}) {
        Session current;
        std::string device;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            current = sessions_[entry.id];
            const auto key = keys_.find(entry.id);
            if (key != keys_.end()) device = key->second;
        }
        Response response;
        std::string address;
        try { address = resolve(entry.address); }
        catch (const std::exception& failure) { return {504, Json({{"error", failure.what()}, {"offline", true}}).dump()}; }
        const auto host = address + ":" + std::to_string(entry.port);
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
        if (!curl) throw std::runtime_error("Cannot start a console request");
        curl_slist* headers = nullptr;
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> owned(nullptr, curl_slist_free_all);
        auto header = [&](const std::string& text) { headers = curl_slist_append(headers, text.c_str()); owned.release(); owned.reset(headers); };
        header("Host: " + host);
        header("Accept: application/json");
        header("Expect:");
        if (with_token && !current.token.empty()) header("X-Archive-Session: " + current.token);
        if (!current.cookie.empty()) header("Cookie: " + std::string(console_cookie) + "=" + current.cookie);
        if (!device.empty()) header("X-ArchiveFetch-Device: " + device);
        if (method == "POST") header(upload ? "Content-Type: application/octet-stream" : "Content-Type: application/json");
        if (upload) header("X-ArchiveFetch-Version: " ARCHIVEFETCH_VERSION);
        const auto url = "http://" + host + path;
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http");
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_NOPROXY, "*");
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 4L);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, timeout);
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "ArchiveFetch/" ARCHIVEFETCH_VERSION);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_body);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, read_header);
        curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &response);
        Upload streamed{{}, progress, 0};
        if (method == "POST" && upload) {
            streamed.input.open(*upload, std::ios::binary);
            if (!streamed.input) return {500, Json({{"error", "The bundled payload cannot be read"}}).dump()};
            curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
            curl_easy_setopt(curl.get(), CURLOPT_READFUNCTION, read_upload);
            curl_easy_setopt(curl.get(), CURLOPT_READDATA, &streamed);
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(std::filesystem::file_size(*upload)));
        } else if (method == "POST") {
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
        }
        const auto code = curl_easy_perform(curl.get());
        if (code != CURLE_OK) {
            const std::string reason = code == CURLE_WRITE_ERROR ? "The console's reply was too large" : std::string("Cannot reach the console: ") + curl_easy_strerror(code);
            return {504, Json({{"error", reason}, {"offline", code != CURLE_WRITE_ERROR}}).dump()};
        }
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response.status);
        if (response.cookie_seen) {
            std::lock_guard<std::mutex> guard(mutex_);
            sessions_[entry.id].cookie = response.cookie.size() <= 128 ? response.cookie : std::string();
        }
        // Plain-text refusals (such as External connections being off) are passed on as a JSON error.
        auto parsed = Json::parse(response.body, nullptr, false);
        if (parsed.is_discarded()) {
            auto text = response.body.substr(0, 500);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            response.body = Json({{"error", text.empty() ? "The console returned HTTP " + std::to_string(response.status) : text}, {"refused", true}}).dump();
        }
        return {static_cast<int>(response.status), response.body};
    }

    Json probe(const ConsoleEntry& entry) {
        const auto reply = send(entry, "GET", "/session", "", false, 4);
        const auto value = Json::parse(reply.body, nullptr, false);
        const auto version = value.is_object() && value.contains("version") && value.at("version").is_string() ? value.at("version").get<std::string>() : std::string();
        Json result = {{"version", version}, {"older", !version.empty() && compare_versions(version, ARCHIVEFETCH_VERSION) < 0},
            {"newer", !version.empty() && compare_versions(version, ARCHIVEFETCH_VERSION) > 0},
            {"selfUpdate", value.is_object() && value.value("selfUpdate", false)}};
        if (reply.status == 200 && value.is_object() && value.contains("token")) {
            result["status"] = "online";
            std::lock_guard<std::mutex> guard(mutex_);
            sessions_[entry.id].token = value.at("token").get<std::string>();
        } else if (reply.status == 401 && value.value("login", false)) result["status"] = "signin";
        else if (value.is_object() && value.value("offline", false)) result["status"] = "offline";
        else { result["status"] = "refused"; result["detail"] = value.is_object() ? value.value("error", "") : ""; }
        return result;
    }

    // The address this device uses for its default route; connecting a UDP socket sends nothing.
    static std::string local_address() {
        const auto probe = socket(AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
        if (probe == INVALID_SOCKET) return {};
#else
        if (probe < 0) return {};
#endif
        sockaddr_in route{};
        route.sin_family = AF_INET;
        route.sin_port = htons(9);
        inet_pton(AF_INET, "192.0.2.1", &route.sin_addr);
        sockaddr_in address{};
        socklen_t length = sizeof(address);
        char text[INET_ADDRSTRLEN]{};
        const bool found = connect(probe, reinterpret_cast<sockaddr*>(&route), sizeof(route)) == 0 &&
            getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length) == 0 &&
            inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text)) != nullptr;
#ifdef _WIN32
        closesocket(probe);
#else
        close(probe);
#endif
        return found ? text : std::string();
    }

    // Only port 8890's /session is asked, with no token, cookie or device key.
    static Json scan_probe(const std::string& address, int port) {
        Response response;
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
        if (!curl) return nullptr;
        const auto url = "http://" + address + ":" + std::to_string(port) + "/session";
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http");
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_NOPROXY, "*");
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 800L);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 2000L);
        curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "ArchiveFetch/" ARCHIVEFETCH_VERSION);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_body);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
        if (curl_easy_perform(curl.get()) != CURLE_OK || response.body.size() > 4096) return nullptr;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response.status);
        const auto value = Json::parse(response.body, nullptr, false);
        if (!value.is_object() || !value.contains("version") || !value.at("version").is_string()) return nullptr;
        const bool online = response.status == 200 && value.contains("token");
        if (!online && !(response.status == 401 && value.value("login", false))) return nullptr;
        const auto version = value.at("version").get<std::string>();
        return {{"address", address}, {"port", port}, {"version", version.substr(0, 32)}, {"status", online ? "online" : "signin"},
            {"older", compare_versions(version, ARCHIVEFETCH_VERSION) < 0}, {"newer", compare_versions(version, ARCHIVEFETCH_VERSION) > 0}};
    }

    // The loader runs whatever ELF arrives on its port, so only the bundled payload is ever sent, and only to a saved console.
    static void send_payload(const std::string& address, int port, const std::filesystem::path& elf, const std::function<void(std::uint64_t)>& progress) {
        std::ifstream input(elf, std::ios::binary);
        if (!input) throw std::runtime_error("The bundled payload cannot be read");
#ifdef _WIN32
        const SOCKET connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (connection == INVALID_SOCKET) throw std::runtime_error("Cannot open a connection");
        const auto finish = [&] { closesocket(connection); };
        const DWORD timeout = 30000;
        setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        constexpr int send_flags = 0;
#else
        const int connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (connection < 0) throw std::runtime_error("Cannot open a connection");
        const auto finish = [&] { close(connection); };
        const timeval timeout{30, 0};
        setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        constexpr int send_flags = MSG_NOSIGNAL;
#endif
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(static_cast<std::uint16_t>(port));
        inet_pton(AF_INET, address.c_str(), &target.sin_addr);
        if (connect(connection, reinterpret_cast<sockaddr*>(&target), sizeof(target)) != 0) {
            finish();
            throw std::runtime_error("Cannot reach the console's loader on port " + std::to_string(port) + "; start the ELF loader on the console and try again");
        }
        std::vector<char> buffer(65536);
        std::uint64_t sent = 0;
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto size = static_cast<std::size_t>(input.gcount());
            for (std::size_t offset = 0; offset < size;) {
                const auto written = ::send(connection, buffer.data() + offset, static_cast<int>(size - offset), send_flags);
                if (written <= 0) { finish(); throw std::runtime_error("The console's loader stopped receiving the payload"); }
                offset += static_cast<std::size_t>(written);
                sent += static_cast<std::uint64_t>(written);
                progress(sent);
            }
        }
        finish();
    }

    void start_install(const std::string& id) {
        if (!payload_available()) throw std::runtime_error("This app has no ArchiveFetch " ARCHIVEFETCH_VERSION " payload to install");
        const auto entry = find(id);
        const auto elf = payload_dir / "ArchiveFetch.elf";
        const auto total = std::filesystem::file_size(elf);
        std::unique_lock<std::mutex> guard(mutex_);
        for (const auto& install : installs_)
            if (install.second.value("running", false)) throw std::runtime_error("Another console is still being updated");
        if (installer_.joinable()) { guard.unlock(); installer_.join(); guard.lock(); }
        installs_[id] = {{"running", true}, {"stage", "sending"}, {"sent", 0}, {"total", total}, {"version", ARCHIVEFETCH_VERSION}};
        installer_ = std::thread([this, entry, elf] {
            const auto update = [this, &entry](const Json& changes) {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& field : changes.items()) installs_[entry.id][field.key()] = field.value();
            };
            try {
                bool through_console = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    const auto known = status_.find(entry.id);
                    through_console = known != status_.end() && known->second.value("selfUpdate", false);
                }
                const auto progress = [this, &entry](std::uint64_t sent) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    installs_[entry.id]["sent"] = sent;
                };
                if (through_console) {
                    // ArchiveFetch on the console hands the payload to its own loader, so the loader port never has to be reachable.
                    update({{"route", "console"}});
                    const auto upload = [&] {
                        auto opened = session(entry);
                        if (opened.status != 200) return opened;
                        return send(entry, "POST", "/api/update", "", true, 600, &elf, progress);
                    };
                    auto reply = upload();
                    // The console restarted and issued a new token: renew it once.
                    if (reply.status == 403) reply = upload();
                    const auto value = Json::parse(reply.body, nullptr, false);
                    std::uint64_t sent = 0;
                    { std::lock_guard<std::mutex> lock(mutex_); sent = installs_[entry.id].value("sent", 0ull); }
                    // A connection lost after the whole payload went out is the old copy stopping, not a failure.
                    const bool handed_over = reply.status == 504 && sent == std::filesystem::file_size(elf);
                    if (reply.status != 200 && !handed_over) {
                        const auto reason = value.is_object() ? value.value("error", "") : "";
                        throw std::runtime_error(reply.status == 401 ? "Sign in to the console first, then update" :
                            !reason.empty() ? reason : "The console did not accept the update (HTTP " + std::to_string(reply.status) + ")");
                    }
                } else {
                    update({{"route", "loader"}});
                    send_payload(resolve(entry.address), entry.loader, elf, progress);
                }
                update({{"stage", "starting"}});
                // The running ArchiveFetch hands over to the new one; it is done once /session reports the new version.
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(install_wait_seconds);
                while (std::chrono::steady_clock::now() < deadline) {
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        if (wake_.wait_for(lock, std::chrono::seconds(2), [this] { return stopping_; })) return;
                    }
                    const auto value = Json::parse(send(entry, "GET", "/session", "", false, 5).body, nullptr, false);
                    if (value.is_object() && value.value("version", "") == ARCHIVEFETCH_VERSION) {
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            sessions_[entry.id].token.clear();
                        }
                        update({{"stage", "syncing"}, {"sent", 0}});
                        try {
                            // Do this on the successor too: an older ArchiveFetch cannot sync the boot copy during handoff.
                            if (!value.value("payloadManagerSync", false))
                                throw std::runtime_error("The console does not support Payload Manager synchronization");
                            const auto sync = [&] {
                                const auto opened = session(entry);
                                if (opened.status != 200) return opened;
                                return send(entry, "POST", "/api/payload-manager/update", "", true, 600, &elf, progress);
                            };
                            auto reply = sync();
                            if (reply.status == 403) reply = sync();
                            const auto result = Json::parse(reply.body, nullptr, false);
                            if (reply.status != 200 || !result.is_object() || !result.contains("replaced") || !result.at("replaced").is_boolean()) {
                                const auto reason = result.is_object() ? result.value("error", std::string()) : std::string();
                                throw std::runtime_error(reason.empty() ? "Payload Manager synchronization returned HTTP " + std::to_string(reply.status) : reason);
                            }
                            update({{"payloadManager", result.at("replaced").get<bool>() ? "updated" : "not-stored"}});
                        } catch (const std::exception& failure) {
                            update({{"warning", "The running ArchiveFetch was updated, but its Payload Manager boot copy was not updated: " +
                                std::string(failure.what()) + ". A restart may bring back the old version."}});
                        }
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            installs_[entry.id]["running"] = false;
                            installs_[entry.id]["stage"] = "done";
                            refresh_ = true;
                        }
                        wake_.notify_all();
                        return;
                    }
                }
                throw std::runtime_error("The payload was sent, but the console did not answer with ArchiveFetch " ARCHIVEFETCH_VERSION " within " +
                    std::to_string(install_wait_seconds / 60) + " minutes");
            } catch (const std::exception& failure) {
                update({{"running", false}, {"stage", "failed"}, {"error", failure.what()}});
            }
        });
    }

    void start_scan() {
        std::vector<std::string> hosts = scan_hosts;
        std::string network;
        if (hosts.empty()) {
            const auto own = local_address();
            in_addr parsed{};
            if (own.empty() || inet_pton(AF_INET, own.c_str(), &parsed) != 1) throw std::runtime_error("This device is not connected to a network");
            const auto ip = ntohl(parsed.s_addr);
            // Home networks only; a scan never leaves the device's own private /24.
            if (!((ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8))
                throw std::runtime_error("Scanning only works on a home network (10.x, 172.16-31.x or 192.168.x); add the console by its address");
            const auto base = ip & 0xFFFFFF00u;
            network = own.substr(0, own.rfind('.')) + ".0/24";
            for (std::uint32_t host = 1; host < 255; ++host) {
                if ((base | host) == ip) continue;
                in_addr next{};
                next.s_addr = htonl(base | host);
                char text[INET_ADDRSTRLEN]{};
                if (inet_ntop(AF_INET, &next, text, sizeof(text))) hosts.emplace_back(text);
            }
        }
        std::unique_lock<std::mutex> guard(mutex_);
        if (scan_.value("running", false)) return;
        if (scanner_.joinable()) { guard.unlock(); scanner_.join(); guard.lock(); }
        scan_ = {{"running", true}, {"found", Json::array()}, {"network", network}, {"checked", 0}, {"total", hosts.size()}};
        scanner_ = std::thread([this, hosts, port = scan_port] {
            std::atomic<std::size_t> next{0};
            std::vector<std::thread> workers;
            for (int worker = 0; worker < 32; ++worker)
                workers.emplace_back([&] {
                    for (std::size_t index = next++; index < hosts.size(); index = next++) {
                        { std::lock_guard<std::mutex> stop(mutex_); if (stopping_) return; }
                        auto found = scan_probe(hosts[index], port);
                        std::lock_guard<std::mutex> update(mutex_);
                        scan_["checked"] = scan_.at("checked").get<std::size_t>() + 1;
                        if (found.is_null()) continue;
                        found["saved"] = std::any_of(consoles_.begin(), consoles_.end(), [&](const ConsoleEntry& entry) {
                            return entry.address == hosts[index] && entry.port == port;
                        });
                        scan_["found"].push_back(found);
                    }
                });
            for (auto& worker : workers) worker.join();
            std::lock_guard<std::mutex> done(mutex_);
            scan_["running"] = false;
        });
    }

    void probe_loop() {
        std::unique_lock<std::mutex> guard(mutex_);
        while (!stopping_) {
            // Checks stop a minute after the page last asked, so a closed or hidden app stays quiet.
            if (std::chrono::steady_clock::now() - last_listed_ > std::chrono::minutes(1)) break;
            const auto consoles = consoles_;
            refresh_ = false;
            guard.unlock();
            std::vector<std::future<Json>> checks;
            for (const auto& entry : consoles) checks.push_back(std::async(std::launch::async, [this, entry] { return probe(entry); }));
            std::map<std::string, Json> results;
            for (std::size_t index = 0; index < consoles.size(); ++index) {
                try { results[consoles[index].id] = checks[index].get(); }
                catch (const std::exception& failure) { results[consoles[index].id] = {{"status", "offline"}, {"detail", failure.what()}}; }
            }
            guard.lock();
            for (auto& result : results) status_[result.first] = std::move(result.second);
            wake_.wait_for(guard, std::chrono::seconds(15), [this] { return stopping_ || refresh_; });
        }
        prober_running_ = false;
    }

    std::filesystem::path file_;
    ConsoleSecrets secrets_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<ConsoleEntry> consoles_;
    // Plain "id:key" pairing keys, in memory only.
    std::map<std::string, std::string> keys_;
    std::map<std::string, Session> sessions_;
    std::map<std::string, Json> status_;
    std::thread prober_;
    std::thread scanner_;
    std::thread installer_;
    std::map<std::string, Json> installs_;
    Json scan_ = {{"running", false}, {"found", Json::array()}};
    bool prober_running_ = false, refresh_ = false, stopping_ = false;
    std::chrono::steady_clock::time_point last_listed_ = std::chrono::steady_clock::now();
};

// Shared by the desktop server and the Android bridge: GET-style list, a console list change, or a relayed request.
Json console_route(ConsoleLink& link, const std::string& route, const Json& body, int& status) {
    status = 200;
    if (route == "/api/consoles") return body.is_object() && body.contains("action") ? link.command(body) : link.list(body.is_object() && body.value("refresh", false));
    const auto reply = link.relay(body.at("console").get<std::string>(), body.value("method", body.contains("body") ? "POST" : "GET"),
        body.at("path").get<std::string>(), body.contains("body") ? body.at("body").dump() : std::string());
    status = reply.status;
    auto value = Json::parse(reply.body, nullptr, false);
    return value.is_discarded() ? Json({{"error", "The console sent an unreadable reply"}}) : value;
}

}
