#pragma once
// The desktop web API test: archive_catalog_web --smoke-test, registered as CTest web_api_tests.
#include "web_application.hpp"
#include "console_link.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// A stand-in ELF loader on loopback: it reads one payload, then the stand-in console reports the new version.
void install_smoke(const std::filesystem::path& root, int console_port, std::mutex& mutex, std::string& console_version) {
    const auto payload = root / "payload";
    std::filesystem::create_directories(payload);
    const std::string elf(300000, 'E');
    std::ofstream(payload / "ArchiveFetch.elf", std::ios::binary) << elf;
    std::ofstream(payload / "ArchiveFetch.version", std::ios::binary) << "0.0.1\n";
    ConsoleLink installer(root / "install-consoles.json");
    installer.payload_dir = payload;
    installer.install_wait_seconds = 10;
    require(!installer.list().at("payload").get<bool>(), "A payload from another version is not offered");
    std::ofstream(payload / "ArchiveFetch.version", std::ios::binary) << ARCHIVEFETCH_VERSION << "\n";
    require(installer.list().at("payload").get<bool>(), "The bundled payload is offered");
#ifdef _WIN32
    const SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    const int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    socklen_t length = sizeof(address);
    require(bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(server, 1) == 0 &&
        getsockname(server, reinterpret_cast<sockaddr*>(&address), &length) == 0, "Stand-in loader started");
    const int loader_port = ntohs(address.sin_port);
    std::atomic<std::size_t> received{0};
    std::thread loader([&] {
        const auto connection = accept(server, nullptr, nullptr);
        char buffer[65536];
        for (int read; (read = static_cast<int>(recv(connection, buffer, sizeof(buffer), 0))) > 0;) received += static_cast<std::size_t>(read);
#ifdef _WIN32
        closesocket(connection);
#else
        close(connection);
#endif
        std::lock_guard<std::mutex> guard(mutex);
        console_version = ARCHIVEFETCH_VERSION;
    });
    const auto id = installer.command({{"action", "add"}, {"name", "Install"}, {"address", "127.0.0.1"}, {"port", console_port}, {"loader", loader_port}})
        .at("consoles").at(0).at("id").get<std::string>();
    installer.command({{"action", "install"}, {"id", id}});
    Json install;
    for (int attempt = 0; attempt < 150; ++attempt) {
        install = installer.list().at("consoles").at(0).value("install", Json::object());
        if (!install.value("running", true)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    loader.join();
#ifdef _WIN32
    closesocket(server);
#else
    close(server);
#endif
    require(install.value("stage", "") == "done" && install.at("sent") == elf.size() && received == elf.size(),
        "Install sends the bundled payload to the loader and waits for the new version");
    std::ifstream saved(root / "install-consoles.json", std::ios::binary);
    require(Json::parse(saved).at("consoles").at(0).at("loader") == loader_port, "A console's loader port is saved");
}

// Two separate backends stand in for this device and a console; the page sends the export of one to the other.
void merge_smoke(const std::filesystem::path& root) {
    const std::string first = "https://archive.org/details/merge-first", second = "https://archive.org/details/merge-second";
    const File alpha{"merge-first", "Alpha.chd", 3, "", "a9993e364706816aba3e25717850c26c9cd0d89d", false};
    const File beta{"merge-second", "Beta.chd", 3, "900150983cd24fb0d6963f7d28e17f72", "", false};
    WebApplication from(root / "merge-from"), into(root / "merge-into");
    SourceEntry one, two;
    one.url = first; one.name = "First"; two.url = second; two.name = "Second"; two.tags = {"Disc"};
    from.model.restore_sources({one, two});
    from.model.add_files({alpha, beta});
    from.command({{"action", "collectionAdd"}, {"urls", {first}}, {"name", "Games"}, {"tags", "add"}});
    from.command({{"action", "collectionEdit"}, {"id", from.collections.at(0).id}, {"name", "Games"}, {"tags", {"Retro"}}});
    from.add_file_tags({alpha}, {"Best"});
    from.add_file_tags({beta}, {"Shared"});
    const auto revision = from.model.snapshot().catalog_revision;
    std::size_t beta_index = 0;
    for (std::size_t index = 0; index < from.model.snapshot().catalog.files.size(); ++index) if (from.model.snapshot().catalog.files[index].name == "Beta.chd") beta_index = index;
    from.command({{"action", "favorite"}, {"index", beta_index}, {"revision", revision}});
    SourceEntry kept;
    kept.url = second; kept.name = "Second (here)"; kept.tags = {"Old"};
    into.model.restore_sources({kept});
    into.model.add_files({beta});
    into.add_file_tags({beta}, {"Mine"});

    auto bundle = from.command({{"action", "mergeExport"}});
    require(bundle.at("sources").size() == 2 && bundle.at("files").size() == 2 && bundle.at("fileTags").size() == 2 && bundle.at("favorites").size() == 1 &&
        bundle.at("collections").at(0).at("name") == "Games" && bundle.dump().find("secret") == std::string::npos, "Merge export lists sources, collections, files, tags and favorites only");
    Json payload = bundle;
    payload["action"] = "mergeImport";
    payload["sourceTags"] = Json::array({{{"url", second}, {"tags", {"Disc"}}}});
    auto result = into.command(payload);
    require(result.at("sources") == 1 && result.at("files") == 1 && result.at("favorites") == 1, "Merge adds only what is missing");
    const auto state = into.model.snapshot();
    const auto merged = std::find_if(state.sources.begin(), state.sources.end(), [&](const SourceEntry& source) { return source.url == second; });
    require(state.sources.size() == 2 && merged != state.sources.end() && merged->name == "Second (here)" && merged->tags == std::vector<std::string>{"Old", "Disc"},
        "Existing sources keep their name and only gain the chosen tags");
    require(into.collections.size() == 1 && into.collections.at(0).name == "Games" && into.collections.at(0).tags == std::vector<std::string>{"Retro"} &&
        into.collections.at(0).items == std::vector<std::string>{"merge-first"}, "New sources join their collection");
    require(state.catalog.files.size() == 2 && into.file_tags.at({"merge-first", "Alpha.chd"}) == std::vector<std::string>{"Best"} &&
        into.file_tags.at({"merge-second", "Beta.chd"}) == std::vector<std::string>{"Mine", "Shared"} && into.favorites.count({"merge-second", "Beta.chd"}),
        "Files, file tags and favorites are added without replacing anything");
    result = into.command(payload);
    require(result.at("sources") == 0 && result.at("files") == 0 && result.at("tags") == 0 && result.at("favorites") == 0, "Merging twice changes nothing");
    from.command({{"action", "selectSources"}, {"urls", {second}}, {"selected", false}});
    bundle = from.command({{"action", "mergeExport"}, {"selected", true}});
    require(bundle.at("sources").size() == 1 && bundle.at("sources").at(0).at("url") == first && bundle.at("files").size() == 1 && bundle.at("favorites").empty(),
        "Merging selected sources exports only those and their files");
    bool rejected = false;
    try { into.command({{"action", "mergeImport"}, {"files", Json::array({{{"item", "merge-first"}, {"name", ""}, {"size", 1}}})}}); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "Invalid merged files are rejected");
}

// A stand-in console on loopback: the same /session, sign-in cookie and session-token rules as the PS5 payload.
void console_smoke(httplib::Client& client, const httplib::Headers& headers, const std::filesystem::path& root) {
    httplib::Server console;
    std::mutex mutex;
    std::string token = "first-token", seen_host, console_version = "0.6.0";
    bool outside = false;
    std::map<std::string, std::string> devices;
    // Callers hold mutex.
    const auto paired = [&](const httplib::Request& request) {
        const auto header = request.get_header_value("X-ArchiveFetch-Device");
        const auto found = devices.find(header.substr(0, 16));
        return header.size() == 81 && found != devices.end() && found->second == header.substr(17);
    };
    const auto signed_in = [&](const httplib::Request& request) { return request.get_header_value("Cookie") == "ArchiveFetchAuth=granted" || paired(request); };
    console.Get("/session", [&](const httplib::Request& request, httplib::Response& response) {
        std::lock_guard<std::mutex> guard(mutex);
        seen_host = request.get_header_value("Host");
        if (outside && !signed_in(request)) { response.status = 401; response.set_content(Json({{"error", "Sign in required"}, {"login", true}, {"version", console_version}, {"pairing", true}}).dump(), "application/json"); return; }
        response.set_content(Json({{"token", token}, {"version", console_version}, {"folderBrowser", true}, {"pairing", true}, {"paired", paired(request)}}).dump(), "application/json");
    });
    console.Post("/api/login", [&](const httplib::Request& request, httplib::Response& response) {
        if (Json::parse(request.body).value("password", "") != "console-password") { response.status = 401; response.set_content(R"({"error":"Wrong password"})", "application/json"); return; }
        response.set_header("Set-Cookie", "ArchiveFetchAuth=granted; Path=/; Max-Age=2592000; HttpOnly; SameSite=Strict");
        response.set_content(R"({"ok":true})", "application/json");
    });
    const auto guarded = [&](const httplib::Request& request, httplib::Response& response) {
        std::lock_guard<std::mutex> guard(mutex);
        if (outside && !signed_in(request)) { response.status = 401; response.set_content(R"({"error":"Sign in required","login":true})", "application/json"); return false; }
        if (request.get_header_value("X-Archive-Session") != token) { response.status = 403; response.set_content(R"({"error":"Access denied"})", "application/json"); return false; }
        return true;
    };
    console.Get("/api/state", [&](const httplib::Request& request, httplib::Response& response) {
        if (guarded(request, response)) response.set_content(R"({"busy":false,"from":"console"})", "application/json");
    });
    console.Post("/api/command", [&](const httplib::Request& request, httplib::Response& response) {
        if (guarded(request, response)) response.set_content(Json({{"echo", Json::parse(request.body)}}).dump(), "application/json");
    });
    console.Post("/api/pair", [&](const httplib::Request& request, httplib::Response& response) {
        if (!guarded(request, response)) return;
        std::lock_guard<std::mutex> guard(mutex);
        const auto id = random_token().substr(0, 16), key = random_token();
        devices[id] = key;
        response.set_content(Json({{"id", id}, {"key", key}, {"name", Json::parse(request.body).at("name")}}).dump(), "application/json");
    });
    console.Get("/api/refused", [](const httplib::Request&, httplib::Response& response) { response.status = 403; response.set_content("Turn on External connections\n", "text/plain"); });
    const int port = console.bind_to_any_port("127.0.0.1");
    require(port > 0, "Stand-in console started");
    std::thread listener([&] { console.listen_after_bind(); });
    console.wait_until_ready();
    try {
        {
            // Loopback only: tests never scan a real network.
            ConsoleLink scanner(root / "scan-consoles.json");
            scanner.scan_hosts = {"127.0.0.1", "127.0.0.2"};
            scanner.scan_port = port;
            scanner.command({{"action", "scan"}});
            Json scan;
            for (int attempt = 0; attempt < 50; ++attempt) {
                scan = scanner.list().at("scan");
                if (!scan.at("running").get<bool>()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            const auto found = scan.at("found");
            require(!scan.at("running").get<bool>() && scan.at("checked") == 2 && found.size() == 1 && found.at(0).at("address") == "127.0.0.1" &&
                found.at(0).at("status") == "online" && found.at(0).at("older") == true && found.at(0).at("saved") == false,
                "A network scan finds ArchiveFetch and skips addresses without it");
        }
        const auto post = [&](const char* path, const Json& value) { return client.Post(path, headers, value.dump(), "application/json"); };
        auto result = post("/api/consoles", {{"action", "add"}, {"name", "Bad"}, {"address", "192.168.1.20/x"}});
        require(result && result->status == 400, "Invalid console addresses rejected");
        result = post("/api/consoles", {{"action", "add"}, {"name", " Living room "}, {"address", "127.0.0.1"}, {"port", port}});
        require(result && result->status == 200, "Console saved");
        auto saved = Json::parse(result->body).at("consoles");
        require(saved.size() == 1 && saved.at(0).at("name") == "Living room", "Console names are trimmed");
        const auto id = saved.at(0).at("id").get<std::string>();
        const auto relay = [&](const std::string& path, const Json& body = nullptr) {
            Json value = {{"console", id}, {"path", path}};
            if (!body.is_null()) value["body"] = body;
            return post("/api/relay", value);
        };
        result = client.Post("/api/relay", Json({{"console", id}, {"path", "/api/state"}}).dump(), "application/json");
        require(result && result->status == 403, "Relay needs the app session");
        result = relay("/api/state");
        require(result && result->status == 200 && Json::parse(result->body).at("from") == "console", "Requests are relayed to the console");
        { std::lock_guard<std::mutex> guard(mutex); require(seen_host == "127.0.0.1:" + std::to_string(port), "The relay sends the console's IP literal as Host"); token = "second-token"; }
        result = relay("/api/command", {{"action", "scan"}});
        require(result && result->status == 200 && Json::parse(result->body).at("echo").at("action") == "scan", "A restarted console's new session token is picked up");
        for (const auto* path : {"/status", "/api/../status", "/api/state#x", "/api/state x"}) {
            result = relay(path); require(result && result->status == 400, "Only console API paths are relayed");
        }
        result = relay("/api/refused");
        require(result && result->status == 502 && Json::parse(result->body).at("error") == "The console refused the request", "A console refusal is not mistaken for a stale app session");
        { std::lock_guard<std::mutex> guard(mutex); require(devices.size() == 1, "The app pairs the first time it controls a console"); outside = true; devices.clear(); }
        result = relay("/api/state");
        require(result && result->status == 401 && Json::parse(result->body).value("login", false), "An outside console asks for sign-in");
        result = relay("/api/login", {{"password", "wrong"}});
        require(result && result->status == 401 && !Json::parse(result->body).value("login", false), "A wrong console password is reported");
        result = relay("/api/login", {{"password", "console-password"}});
        require(result && result->status == 200, "Console sign-in relayed");
        result = relay("/api/state");
        require(result && result->status == 200, "The sign-in cookie is kept for later requests");
        {
            std::lock_guard<std::mutex> guard(mutex);
            require(devices.size() == 1, "The app pairs once it may control the console");
        }
        {
            std::ifstream input(root / "consoles.json", std::ios::binary);
            const std::string saved((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            std::lock_guard<std::mutex> guard(mutex);
            require(saved.find("\"pairing\"") != std::string::npos && saved.find(devices.begin()->second) == std::string::npos, "The pairing key is saved protected");
        }
        {
            // A restarted app has no sign-in cookie; the saved pairing key is enough.
            ConsoleLink restarted(root / "consoles.json", platform_console_secrets());
            require(restarted.relay(id, "GET", "/api/state", "").status == 200, "A paired app connects from outside without the password");
            { std::lock_guard<std::mutex> guard(mutex); devices.clear(); }
            require(restarted.relay(id, "GET", "/api/state", "").status == 401, "A device removed on the console has to sign in again");
            require(restarted.relay(id, "GET", "/session", "").status == 401, "A removed device does not pair again on its own");
            { std::lock_guard<std::mutex> guard(mutex); require(devices.empty(), "A removed device does not pair again on its own"); }
            require(restarted.relay(id, "POST", "/api/login", Json({{"password", "console-password"}}).dump()).status == 200, "Signing in again");
            std::lock_guard<std::mutex> guard(mutex);
            require(devices.size() == 1, "Signing in with the password pairs again");
        }
        result = post("/api/relay", {{"console", "0123456789abcdef"}, {"path", "/api/state"}});
        require(result && result->status == 400, "Unknown consoles rejected");
        result = post("/api/consoles", {{"action", "add"}, {"name", "Off"}, {"address", "127.0.0.1"}, {"port", 1}});
        require(result && result->status == 200, "Second console saved");
        const auto offline = Json::parse(result->body).at("consoles").at(1).at("id").get<std::string>();
        result = post("/api/relay", {{"console", offline}, {"path", "/api/state"}});
        require(result && result->status == 504 && Json::parse(result->body).value("offline", false), "An unreachable console is reported");
        Json listed;
        for (int attempt = 0; attempt < 50; ++attempt) {
            result = client.Get("/api/consoles", headers);
            listed = Json::parse(result->body).at("consoles");
            if (listed.at(0).at("status") != "checking" && listed.at(1).at("status") != "checking") break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        require(listed.at(0).at("status") == "online" && listed.at(0).at("version") == "0.6.0" && listed.at(0).at("older") == true &&
            listed.at(1).at("status") == "offline", "Console status shows online, older version and offline");
        result = post("/api/consoles", {{"action", "edit"}, {"id", id}, {"name", "Bedroom"}, {"address", "localhost"}, {"port", port}});
        require(result && result->status == 200 && Json::parse(result->body).at("consoles").at(0).at("name") == "Bedroom", "Console edited");
        result = relay("/api/state");
        require(result && result->status == 401, "Editing a console forgets its sign-in");
        { std::lock_guard<std::mutex> guard(mutex); require(seen_host == "127.0.0.1:" + std::to_string(port), "Host names are sent as the resolved IP"); }
        post("/api/consoles", {{"action", "remove"}, {"id", offline}});
        std::ifstream input(root / "consoles.json", std::ios::binary);
        const auto file = Json::parse(input).at("consoles");
        require(file.size() == 1 && file.at(0).at("name") == "Bedroom" && file.dump().find("granted") == std::string::npos, "Consoles are saved without sign-ins");
        install_smoke(root, port, mutex, console_version);
    } catch (...) { console.stop(); listener.join(); throw; }
    console.stop(); listener.join();
}

void web_smoke(httplib::Server& server, WebApplication& application, int port, const std::string& session) {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(5, 0); client.set_read_timeout(10, 0);
    httplib::Headers headers{{"X-Archive-Session", session}};
    auto send = [&](const Json& value) { return client.Post("/api/command", headers, value.dump(), "application/json"); };
    auto result = client.Get("/");
    require(result && result->status == 200 && result->body.find("<title>ArchiveFetch</title>") != std::string::npos, "Bundled HTML served");
    require(result->body.find("id=\"get-s3-keys\"") != std::string::npos, "Settings includes the S3 key shortcut");
    for (const auto* script : {"/app-core.js", "/app-library.js", "/app-sources.js", "/app-columns.js", "/app-dialogs.js", "/app-consoles.js", "/app-main.js"}) {
        result = client.Get(script); require(result && result->status == 200, "Bundled scripts served");
    }
    result = client.Get("/session");
    require(result && result->status == 200 && Json::parse(result->body).at("version") == ARCHIVEFETCH_VERSION, "The session reports the app version");
    result = client.Get("/assets/Karla.ttf"); require(result && result->status == 200 && result->body.size() > 10000, "Bundled font served");
    result = client.Get("/api/state"); require(result && result->status == 403, "Unauthenticated API rejected");
    result = client.Post("/api/command", "{}", "application/json"); require(result && result->status == 403, "Unauthenticated mutation rejected");
    auto hostile = headers; hostile.emplace("Origin", "https://example.invalid");
    result = client.Post("/api/command", hostile, "{\"action\":\"clearSources\"}", "application/json"); require(result && result->status == 403, "Hostile origin rejected");
    hostile = headers; hostile.emplace("Host", "example.invalid");
    result = client.Get("/api/state", hostile); require(result && result->status == 403, "Hostile Host rejected");
    result = client.Post("/api/command", headers, "{", "application/json"); require(result && result->status == 400, "Malformed JSON rejected");
    std::atomic<int> browser_requests{0};
    application.open_s3_keys = [&] { ++browser_requests; };
    result = send({{"action", "openS3Keys"}, {"url", "https://example.invalid"}});
    require(result && result->status == 200 && browser_requests == 1, "Authenticated S3 key shortcut uses the native browser action");
    result = client.Post("/api/command", "{\"action\":\"openS3Keys\"}", "application/json");
    require(result && result->status == 403 && browser_requests == 1, "Unauthenticated requests cannot open the browser");
    result = send({{"action", "settings"}, {"destination", (application.root / "downloads").u8string()},
        {"output", (application.root / "exports").u8string()}, {"automatic", false}, {"access", "test-access"}, {"secret", "test-secret"}, {"remember", false}});
    require(result && result->status == 200, "Settings saved");
    result = client.Get("/api/state", headers);
    require(result && result->status == 200 && result->body.find("test-secret") == std::string::npos && result->body.find("test-access") == std::string::npos, "Credentials never returned");
    result = client.Get("/api/library", headers);
    require(result && result->status == 200 && Json::parse(result->body).at("files").size() == 2, "Live model catalog returned");
    {
        const Json base = {{"action", "settings"}, {"destination", (application.root / "downloads").u8string()},
            {"output", (application.root / "exports").u8string()}, {"automatic", false}, {"remember", false}};
        auto excluded = base; excluded["excludedExtensions"] = "*.chd";
        result = send(excluded); require(result && result->status == 400 && application.config.excluded_extensions == default_excluded_extensions(), "Invalid hidden types rejected");
        excluded["excludedExtensions"] = "CHD, .txt";
        result = send(excluded); require(result && result->status == 200, "Hidden types saved");
        result = client.Get("/api/library", headers);
        const auto hidden = Json::parse(result->body).at("files");
        result = client.Get("/api/state", headers);
        const auto snapshot = Json::parse(result->body);
        require(hidden.size() == 1 && hidden.at(0).at("name") == "Alpha.chd" && snapshot.at("count") == 0 && snapshot.at("settings").at("excludedExtensions") == ".chd, .txt",
            "Hidden types leave only downloaded files of that type in the Library");
        require(read_config(application.settings, true).excluded_extensions == std::vector<std::string>{".chd", ".txt"}, "Hidden types are saved with the settings");
        excluded["excludedExtensions"] = "";
        result = send(excluded); require(result && result->status == 200 && application.config.excluded_extensions.empty(), "Hidden types can be cleared");
        result = client.Get("/api/library", headers); require(Json::parse(result->body).at("files").size() == 2, "Clearing hidden types shows every file again");
        result = send({{"action", "hideExtension"}, {"extension", "CHD"}});
        require(result && result->status == 200 && Json::parse(result->body).at("excludedExtensions") == ".chd" &&
            read_config(application.settings, true).excluded_extensions == std::vector<std::string>{".chd"}, "One file type can be hidden and is saved");
        result = send({{"action", "hideExtension"}, {"extension", ".chd .txt"}});
        require(result && result->status == 400 && application.config.excluded_extensions.size() == 1, "Hiding takes exactly one file type");
        result = send({{"action", "showExtension"}, {"extension", ".chd"}});
        require(result && result->status == 200 && application.config.excluded_extensions.empty() &&
            read_config(application.settings, true).excluded_extensions.empty(), "A hidden file type can be shown again");
        excluded["excludedExtensions"] = format_extension_list(default_excluded_extensions());
        result = send(excluded); require(result && result->status == 200, "Default hidden types restored");
    }
    result = send({{"action", "sourceTags"}, {"url", "https://archive.org/details/archive-web-test"}, {"tags", {"PS2", "A & B", "ps2"}}});
    require(result && result->status == 200, "Source tags updated through the API");
    result = client.Get("/api/state", headers);
    require(result && Json::parse(result->body).at("sources").at(0).at("tags") == Json({"PS2", "A & B"}), "Source API returns normalized tags");
    result = client.Get("/api/library", headers);
    require(result && Json::parse(result->body).at("files").at(0).at("sourceTags") == Json({"PS2", "A & B"}), "Library inherits its source tags");
    result = send({{"action", "sourceTags"}, {"url", "https://archive.org/details/archive-web-test"}, {"tags", {std::string(129, 'x')}}});
    require(result && result->status == 400, "Oversized tags rejected by API");
    result = send({{"action", "addSourceTags"}, {"urls", {"https://archive.org/details/archive-web-test"}}, {"tags", {"ps2", "Retro"}}});
    require(result && result->status == 200 && application.model.snapshot().sources[0].tags == std::vector<std::string>{"PS2", "A & B", "Retro"},
        "Bulk source tags add to existing tags without case-insensitive duplicates");
    result = send({{"action", "addSourceTags"}, {"urls", {"https://archive.org/details/missing-source"}}, {"tags", {"Retro"}}});
    require(result && result->status == 400, "Bulk source tags reject unknown sources");
    result = send({{"action", "sourceTags"}, {"url", "https://archive.org/details/archive-web-test"}, {"tags", {"PS2", "A & B"}}});
    require(result && result->status == 200, "Source tags restored after bulk test");
    result = send({{"action", "selectSources"}, {"urls", Json::array()}, {"selected", false}});
    require(result && result->status == 200 && application.model.snapshot().sources[0].selected, "Filtered source selection leaves hidden sources unchanged");
    auto revision = application.model.snapshot().catalog_revision;
    result = send({{"action", "fileTags"}, {"index", 0}, {"revision", revision + 1}, {"tags", {"Completed"}}});
    require(result && result->status == 400, "Stale file tag selection rejected");
    result = send({{"action", "fileTags"}, {"index", 0}, {"revision", revision}, {"tags", {" Completed ", "completed", "A & B"}}});
    require(result && result->status == 200, "File tags saved");
    result = client.Get("/api/library", headers);
    const auto tagged = Json::parse(result->body);
    require(tagged.at("files").at(0).at("tags") == Json({"Completed", "A & B"}) && tagged.at("files").at(1).at("tags").empty(), "Custom tags apply only to the chosen file");
    require(tagged.at("files").at(0).at("sourceTags") == Json({"PS2", "A & B"}) && tagged.at("tagRevision") == 1, "File tag changes preserve inheritance and advance the revision");
    result = send({{"action", "fileTags"}, {"index", 0}, {"revision", revision}, {"tags", {std::string(129, 'x')}}});
    require(result && result->status == 400, "Oversized file tags rejected");
    result = send({{"action", "addFileTags"}, {"indices", {0, 1}}, {"revision", revision + 1}, {"tags", {"Batch"}}});
    require(result && result->status == 400, "Stale bulk tag selection rejected");
    result = send({{"action", "addFileTags"}, {"indices", {0, 1}}, {"revision", revision}, {"tags", {"batch", "A & B"}}});
    require(result && result->status == 200, "Bulk file tags saved");
    result = client.Get("/api/library", headers);
    const auto bulk = Json::parse(result->body);
    require(bulk.at("files").at(0).at("tags") == Json({"Completed", "A & B", "batch"}) && bulk.at("files").at(1).at("tags") == Json({"batch", "A & B"}),
        "Bulk tags add to existing tags without case-insensitive duplicates");
    result = send({{"action", "addFileTags"}, {"indices", {0, 1}}, {"revision", revision}, {"tags", Json::array()}});
    require(result && result->status == 400, "Bulk tagging needs a tag");
    result = send({{"action", "fileTags"}, {"index", 0}, {"revision", revision}, {"tags", {"Completed", "A & B"}}});
    require(result && result->status == 200, "File tags restored after bulk test");
    result = send({{"action", "fileTags"}, {"index", 1}, {"revision", revision}, {"tags", Json::array()}});
    require(result && result->status == 200, "Bulk tags removed again");
    result = send({{"action", "favorite"}, {"index", 0}, {"revision", revision + 1}}); require(result && result->status == 400, "Stale favorite rejected");
    result = send({{"action", "favorite"}, {"index", 0}, {"revision", revision}}); require(result && result->status == 200, "Favorite saved");
    result = send({{"action", "enqueue"}, {"indices", {0}}, {"revision", revision}});
    require(result && result->status == 200 && Json::parse(result->body).contains("confirmOverwrite") && application.model.snapshot().queue.empty(), "Overwrite requires approval");
    result = send({{"action", "enqueue"}, {"indices", {1}}, {"revision", revision + 1}}); require(result && result->status == 400, "Stale queue selection rejected");
    result = send({{"action", "enqueue"}, {"indices", {1}}, {"revision", revision}});
    require(result && result->status == 200 && application.model.snapshot().queue.size() == 1, "Queue uses real model");
    auto id = application.model.snapshot().queue.front().id;
    result = send({{"action", "removeQueue"}, {"id", id}}); require(result && result->status == 200 && application.model.snapshot().queue.empty(), "Stable queue removal");
    result = send({{"action", "enqueue"}, {"indices", {1}}, {"revision", revision}});
    require(result && result->status == 200, "Retry fixture queued");
    application.model.start_downloads(application.config, [](const File&, const std::filesystem::path&, bool, const TransferHooks&) { throw std::runtime_error("Retry fixture failure"); });
    application.model.wait();
    const auto retry_id = application.model.snapshot().queue.front().id;
    application.download = [](const File& file, const std::filesystem::path& directory, bool, const TransferHooks&) {
        std::filesystem::create_directories(directory);
        std::ofstream output(directory / std::filesystem::u8path(file.name).filename(), std::ios::binary); output << "abc";
    };
    result = send({{"action", "retryQueue"}, {"id", retry_id}});
    require(result && result->status == 200, "Failed entry retry accepted");
    application.model.wait();
    require(application.model.snapshot().queue.empty(), "Successful per-entry retry removed from queue");
    result = send({{"action", "retryQueue"}, {"id", retry_id}});
    require(result && result->status == 400, "Stale retry ID rejected");
    result = send({{"action", "retryQueue"}, {"id", 0}});
    require(result && result->status == 400, "Retry requires a specific queue ID");
    result = send({{"action", "forget"}}); require(result && result->status == 200, "Session credentials cleared");
    application.persist();
    WebApplication restored(application.root);
    require(restored.model.snapshot().catalog.files.size() == 2 && restored.favorites.size() == 1 && !restored.config.automatically_start_downloads, "Library favorites and settings survive restart");
    require(restored.config.s3.secret_key.empty(), "Session-only credentials not persisted");
    require(restored.file_tags == application.file_tags, "File tags survive application restart");
    auto reordered = restored.model.snapshot().catalog;
    std::reverse(reordered.files.begin(), reordered.files.end());
    save_catalog(reordered, application.root / "tag-reorder.xml");
    restored.model.import_catalog(application.root / "tag-reorder.xml"); restored.model.wait();
    const auto reordered_library = restored.library();
    for (const auto& file : reordered_library.at("files")) {
        require(file.at("tags") == (file.at("name") == "Alpha.chd" ? Json({"Completed", "A & B"}) : Json::array()), "File tags follow identity after catalog reordering");
    }
    restored.save_file_tags(reordered.files.front(), {"Temporary"});
    restored.save_file_tags(reordered.files.front(), {});
    const auto cleared_library = restored.library();
    for (const auto& file : cleared_library.at("files")) if (file.at("name") == reordered.files.front().name && file.at("item") == reordered.files.front().item) {
        require(file.at("tags").empty(), "Clearing file tags leaves inherited tags intact");
        require(!file.at("sourceTags").empty(), "Source tags remain after file tags are cleared");
    }
    restored.save_file_tags(application.model.snapshot().catalog.files.at(0), {"Completed", "A & B"});
    require(restored.model.snapshot().sources.at(0).tags == std::vector<std::string>({"PS2", "A & B"}), "Source tags survive application restart");
    {
        using Tags = std::vector<std::string>;
        WebApplication grouped(application.root / "collection-tests");
        const std::string a = "https://archive.org/details/col-a", b = "https://archive.org/details/col-b", c = "https://archive.org/details/col-c";
        grouped.model.restore_sources({{a, "A", true, {}, {}, {"PS2", "Retro"}}, {b, "B", false, {}, {}, {"Music"}}, {c, "C", false, {}, {}, {}}});
        const auto tags_of = [&](std::size_t index) { return grouped.model.snapshot().sources.at(index).tags; };
        const auto throws = [&](const Json& value) { try { grouped.command(value); return false; } catch (const std::exception&) { return true; } };
        const auto games = grouped.command({{"action", "collectionAdd"}, {"urls", {a, b}}, {"name", "Games"}, {"tags", "choose"}, {"keep", {"retro", "Music"}}}).at("id").get<std::string>();
        require(tags_of(0) == Tags{"Retro", "Music"} && tags_of(1) == Tags{"Retro", "Music"} && tags_of(2).empty(), "Joining keeps the chosen tags and members share them");
        require(throws({{"action", "sourceTags"}, {"url", a}, {"tags", {"X"}}}) && throws({{"action", "addSourceTags"}, {"urls", {c, a}}, {"tags", {"X"}}}) && tags_of(2).empty(),
            "Sources in a collection cannot be tagged directly");
        require(throws({{"action", "collectionAdd"}, {"urls", {c}}, {"name", "games"}, {"tags", "add"}}), "Collection names are unique ignoring case");
        grouped.command({{"action", "collectionEdit"}, {"id", games}, {"name", "Retro games"}, {"tags", {"PS2"}}});
        require(tags_of(0) == Tags{"PS2"} && tags_of(1) == Tags{"PS2"}, "Editing a collection retags its members");
        const auto music = grouped.command({{"action", "collectionAdd"}, {"urls", {b}}, {"name", "Music"}, {"tags", "discard"}}).at("id").get<std::string>();
        require(tags_of(1).empty() && grouped.collection_of("col-b")->id == music && grouped.collection_of("col-a")->items == Tags{"col-a"}, "Adding to another collection moves the source");
        grouped.command({{"action", "collectionRemove"}, {"urls", {a}}, {"tags", "copy"}});
        require(tags_of(0) == Tags{"PS2"} && !grouped.collection_of("col-a"), "Leaving can copy the collection's tags");
        grouped.command({{"action", "collectionAdd"}, {"urls", {a}}, {"id", games}, {"tags", "add"}});
        const auto listed = grouped.snapshot();
        require(listed.at("sources").at(0).at("collection") == games && listed.at("collections").size() == 2, "State lists collections and membership");
        grouped.command({{"action", "collectionDelete"}, {"id", music}, {"tags", "remove"}});
        require(grouped.collections.size() == 1 && tags_of(1).empty(), "Deleting a collection releases its sources");
        grouped.model.set_source_tags(a, {"Edited elsewhere"}); grouped.snapshot();
        require(tags_of(0) == Tags{"PS2"}, "Member tags changed outside the collection are repaired");
        grouped.command({{"action", "removeSource"}, {"index", 0}}); grouped.snapshot();
        require(grouped.collections.front().items.empty(), "Removed sources leave their collection");
        WebApplication reopened(grouped.root);
        require(reopened.collections.size() == 1 && reopened.collections.front().name == "Retro games" && reopened.collections.front().tags == Tags{"PS2"},
            "Collections survive restart");
    }
    {
        WebApplication parallel(application.root / "parallel-tests");
        save_catalog(application.model.snapshot().catalog, parallel.root / "fixture.xml");
        parallel.model.import_catalog(parallel.root / "fixture.xml"); parallel.model.wait();
        const std::string first = "https://archive.org/details/archive-web-test", added = "https://archive.org/details/parallel-added";
        parallel.model.restore_sources({{first, "Test", true, {}, {}, {}}});
        parallel.model.enqueue({0});
        auto config = parallel.config; config.download_dir = parallel.root / "downloads"; config.s3 = {};
        // The first file waits for release_first; the later fixture file waits for release_later; the added one fails once.
        std::atomic<bool> release_first{false}, release_later{false};
        std::atomic<int> added_attempts{0};
        std::mutex calls_mutex;
        std::vector<std::string> calls;
        const auto first_name = parallel.model.snapshot().queue.front().file.name;
        parallel.model.start_downloads(config, [&](const File& file, const std::filesystem::path&, bool, const TransferHooks& hooks) {
            { std::lock_guard<std::mutex> guard(calls_mutex); calls.push_back(file.name); }
            if (file.item == "parallel-added") {
                if (added_attempts++ == 0) throw std::runtime_error("Retry fixture failure");
                return;
            }
            const auto& release = file.name == first_name ? release_first : release_later;
            while (!release && !hooks.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        });
        const auto fetch = [](const std::string& url) {
            return std::string(url.find("_meta.xml") != std::string::npos ? "<metadata><title>Added</title></metadata>" :
                "<files><file name='New source.chd'><size>9</size><sha1>a9993e364706816aba3e25717850c26c9cd0d89d</sha1></file></files>");
        };
        const auto scanned = [&] {
            for (int attempt = 0; attempt < 1000 && parallel.model.snapshot().scanning; ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return !parallel.model.snapshot().scanning;
        };
        parallel.command({{"action", "sourceTags"}, {"url", first}, {"tags", {"PS3"}}});
        parallel.model.add_source(added, false, {}, fetch);
        require(scanned() && parallel.model.snapshot().sources.size() == 2, "Sources can be added while downloading");
        parallel.command({{"action", "selectSources"}, {"urls", {first}}, {"selected", false}});
        parallel.model.scan_selected_sources(config, fetch);
        require(scanned(), "Scan finishes while downloading");
        auto state = parallel.model.snapshot();
        require(state.downloading && state.queue.size() == 1 && state.queue.front().status == QueueStatus::Active && state.sources[0].tags == std::vector<std::string>{"PS3"} &&
            std::any_of(state.catalog.files.begin(), state.catalog.files.end(), [](const File& file) { return file.item == "parallel-added"; }),
            "Scanning and tagging sources leave the running download alone");
        bool rejected = false;
        try { parallel.command({{"action", "favorite"}, {"index", 0}, {"revision", state.catalog_revision}}); } catch (const std::exception&) { rejected = true; }
        require(rejected, "Library changes still wait for downloads");

        std::size_t added_index = 0, later_index = 0;
        for (std::size_t index = 0; index < state.catalog.files.size(); ++index) {
            const auto& file = state.catalog.files[index];
            if (file.item == "parallel-added") added_index = index;
            else if (file.name != first_name) later_index = index;
        }
        // The failing file goes first, so it fails while the later one is still downloading.
        for (const auto index : {added_index, later_index}) {
            const auto queued = parallel.command({{"action", "enqueue"}, {"indices", {index}}, {"revision", state.catalog_revision}});
            require(!queued.contains("confirmOverwrite") && !queued.contains("locationConflicts"), "Queueing while a download runs needs no prompt");
        }
        require(parallel.model.snapshot().queue.size() == 3, "Files can be queued while a download runs");
        release_first = true;
        const auto wait_for = [&](const std::function<bool(const GuiSnapshot&)>& condition) {
            for (int attempt = 0; attempt < 1000; ++attempt) {
                if (condition(parallel.model.snapshot())) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return false;
        };
        std::uint64_t failed_id = 0;
        require(wait_for([&](const GuiSnapshot& now) {
            bool later_active = false;
            for (const auto& entry : now.queue) {
                if (entry.file.item == "parallel-added" && entry.status == QueueStatus::Failed) failed_id = entry.id;
                if (entry.file.item != "parallel-added" && entry.file.name != first_name && entry.status == QueueStatus::Active) later_active = true;
            }
            return failed_id && later_active;
        }), "The running queue takes files added after it started");
        parallel.command({{"action", "retryQueue"}, {"id", failed_id}});
        release_later = true;
        parallel.model.wait();
        state = parallel.model.snapshot();
        require(state.queue.empty() && !state.busy && added_attempts == 2 && calls.size() == 4,
            "A file retried while the queue runs is downloaded again before it finishes");
    }
    {
        WebApplication cleaning(application.root / "clean-tests");
        save_catalog(application.model.snapshot().catalog, cleaning.root / "fixture.xml");
        const auto load = [&] { cleaning.model.import_catalog(cleaning.root / "fixture.xml"); cleaning.model.wait(); return cleaning.model.snapshot().catalog.files; };
        auto files = load();
        const auto folder = cleaning.root / "downloads";
        const auto copy = folder / std::filesystem::u8path(files[0].name).filename();
        const auto download_first = [&] { std::filesystem::create_directories(folder); std::ofstream(copy, std::ios::binary) << "abc"; cleaning.record_download(files[0], folder); };
        const auto clean = [&](Json value) { value["action"] = "cleanLibrary"; return cleaning.command(value); };
        const auto throws = [&](const Json& value) { try { clean(value); return false; } catch (const std::exception&) { return true; } };
        download_first();
        auto preview = clean({{"preview", true}});
        require(preview.at("total") == 2 && preview.at("orphans") == 1 && preview.at("undownloaded") == 1 && preview.at("all") == 2 && preview.at("delete").at("files") == 1,
            "Clean preview counts each option");
        preview = clean({{"preview", true}, {"indices", {0}}, {"revision", cleaning.model.snapshot().catalog_revision}});
        require(preview.at("total") == 1 && preview.at("orphans") == 0 && preview.at("undownloaded") == 0, "Selected scope counts only the chosen files");
        auto unplugged = folder; unplugged += "-unplugged";
        std::filesystem::rename(folder, unplugged);
        preview = clean({{"preview", true}});
        require(preview.at("orphans") == 1 && preview.at("undownloaded") == 1 && preview.at("delete").at("files") == 0 && preview.at("delete").at("keptCount") == 1,
            "A copy on an unavailable location still counts as downloaded and is never deleted");
        std::filesystem::rename(unplugged, folder);
        require(clean({{"mode", "orphans"}}).at("removed") == 1 && cleaning.model.snapshot().catalog.files.size() == 1, "Clearing files without sources keeps downloaded ones");
        const auto listed = cleaning.library().at("files").at(0);
        require(listed.at("sourceRemoved") && listed.at("downloaded"), "Library marks files whose source was removed");
        cleaning.command({{"action", "deleteDownload"}, {"revision", cleaning.model.snapshot().catalog_revision}, {"index", 0}, {"path", copy.u8string()}, {"removeEntry", true}});
        require(!std::filesystem::exists(copy) && cleaning.model.snapshot().catalog.files.empty(), "Deleting a file whose source was removed can also remove its entry");
        files = load();
        download_first();
        require(throws({{"mode", "delete"}}), "Deleting everything needs the typed confirmation");
        const auto wiped = clean({{"mode", "delete"}, {"confirm", "DELETE"}});
        require(wiped.at("deleted") == 1 && !std::filesystem::exists(copy) && cleaning.model.snapshot().catalog.files.empty() && cleaning.download_state.at("records").empty(),
            "Clear and delete removes downloaded copies, their records and every entry");
        files = load();
        const auto partial_folder = std::filesystem::absolute(cleaning.config.download_dir);
        std::filesystem::create_directories(partial_folder);
        const auto partial = partial_folder / (std::filesystem::u8path(files[0].name).filename().u8string() + ".part");
        std::ofstream(partial, std::ios::binary) << "a";
        require(clean({{"mode", "undownloaded"}}).at("removed") == 1, "Partly downloaded files are kept by Clear undownloaded files");
        std::filesystem::remove(partial);
        require(clean({{"mode", "all"}}).at("removed") == 1 && cleaning.model.snapshot().catalog.files.empty(), "Clear all removes every entry");
    }
    {
        WebApplication locations(application.root / "location-tests");
        locations.model.restore_sources(application.model.snapshot().sources);
        save_catalog(application.model.snapshot().catalog, locations.root / "fixture.xml");
        locations.model.import_catalog(locations.root / "fixture.xml"); locations.model.wait();
        const auto files = locations.model.snapshot().catalog.files;
        const auto location_settings = [&](const std::string& preference, const std::string& fallback = "default") {
            locations.command({{"action", "settings"}, {"destination", (locations.root / "downloads").u8string()}, {"output", (locations.root / "exports").u8string()},
                {"automatic", false}, {"defaultLocation", fallback}, {"locationPreference", preference}});
        };
        const auto add_location = [&](const std::string& name, const std::vector<std::string>& tags) {
            return locations.command({{"action", "addLocation"}, {"name", name}, {"path", (locations.root / name).u8string()}, {"tags", tags}}).at("id").get<std::string>();
        };
        const auto file_location = add_location("File games", {"Completed"});
        const auto source_location = add_location("Source games", {"PS2"});
        const auto other_location = add_location("Other games", {"Other"});
        const auto rejects = [&](const Json& command) {
            try { locations.command(command); return false; } catch (const std::exception&) { return true; }
        };
        require(rejects({{"action", "removeLocation"}, {"id", "default"}}), "Built-in Default cannot be removed");
        require(rejects({{"action", "editLocation"}, {"id", "default"}, {"name", "Renamed"}, {"path", locations.root.u8string()}}), "Built-in Default cannot be renamed");
        require(rejects({{"action", "addLocation"}, {"name", "Duplicate"}, {"path", locations.root.u8string()}, {"tags", {"pS2"}}}), "Tags have one owner ignoring case");
        require(rejects({{"action", "addLocation"}, {"name", "FILE GAMES"}, {"path", locations.root.u8string()}}), "Nicknames are unique ignoring case");
        require(rejects({{"action", "addLocation"}, {"name", "Relative"}, {"path", "relative/path"}}), "Relative paths are rejected");
        require(locations.settings_json().at("locations").size() == 4, "Rejected location changes are atomic");
        locations.save_file_tags(files.at(0), {"Completed"});
        const auto enqueue_location = [&](const std::string& selection, const Json& choices = Json::object()) {
            return locations.command({{"action", "enqueue"}, {"revision", locations.model.snapshot().catalog_revision}, {"indices", {0}}, {"location", selection}, {"locationChoices", choices}});
        };
        const auto queued_at = [&](const std::string& id) {
            const auto queued = locations.model.snapshot().queue;
            const auto target = std::filesystem::weakly_canonical(std::filesystem::u8path(find_location(locations.download_state, id).at("path").get<std::string>()));
            require(queued.size() == 1 && queued.front().destination == target, "Resolved queue destination matches priority");
            locations.model.remove_entry(queued.front().id);
        };
        for (const auto& preference : {"auto", "file"}) { location_settings(preference); enqueue_location("auto"); queued_at(file_location); }
        location_settings("source"); enqueue_location("auto"); queued_at(source_location);
        locations.save_file_tags(files.at(0), {});
        location_settings("file"); enqueue_location("auto"); queued_at(source_location);
        locations.model.set_source_tags("https://archive.org/details/archive-web-test", {});
        locations.save_file_tags(files.at(0), {"Completed"});
        location_settings("source"); enqueue_location("auto"); queued_at(file_location);
        locations.save_file_tags(files.at(0), {}); enqueue_location("auto"); queued_at("default");
        locations.save_file_tags(files.at(0), {"Completed"});
        location_settings("default", source_location); enqueue_location("auto"); queued_at(source_location);
        enqueue_location(other_location); queued_at(other_location);
        enqueue_location("fallback"); queued_at(source_location);
        const auto other_directory = std::filesystem::u8path(find_location(locations.download_state, other_location).at("path").get<std::string>());
        std::filesystem::create_directories(other_directory);
        const auto other_file = other_directory / std::filesystem::u8path(files[0].name).filename();
        std::ofstream(other_file, std::ios::binary) << "abc";
        const auto replacement = enqueue_location(other_location);
        require(replacement.contains("confirmOverwrite") && replacement.at("confirmOverwrite").at(0) == other_file.u8string() && locations.model.snapshot().queue.empty(), "Overwrite approval checks the resolved location, not the default");
        std::filesystem::remove(other_file);
        location_settings("auto");
        locations.save_file_tags(files.at(0), {"Completed", "Other"});
        require(enqueue_location("auto").contains("locationConflicts") && locations.model.snapshot().queue.empty(), "Ambiguous tags do not enqueue anything");
        require(rejects({{"action", "enqueue"}, {"revision", locations.model.snapshot().catalog_revision}, {"indices", {0}}, {"location", "auto"}, {"locationChoices", {{"0", source_location}}}}), "Conflict choices must match a candidate");
        enqueue_location("auto", {{"0", other_location}}); queued_at(other_location);
        locations.save_file_tags(files.at(0), {"Completed"});
        locations.model.set_source_tags("https://archive.org/details/archive-web-test", {"PS2"});
        locations.command({{"action", "enqueue"}, {"revision", locations.model.snapshot().catalog_revision}, {"indices", {0, 1}}, {"location", "auto"}});
        const auto queued = locations.model.snapshot().queue;
        require(queued.size() == 2 && queued[0].destination != queued[1].destination, "Mixed selection routes independently");
        locations.command({{"action", "editLocation"}, {"id", file_location}, {"name", "File games"}, {"path", (locations.root / "Changed folder").u8string()}, {"tags", {"Completed"}}});
        locations.command({{"action", "removeLocation"}, {"id", source_location}});
        require(locations.model.snapshot().queue[0].destination == queued[0].destination && locations.model.snapshot().queue[1].destination == queued[1].destination, "Editing and removing locations cannot retarget queued files");
        locations.download = application.download;
        locations.command({{"action", "start"}}); locations.model.wait();
        require(locations.model.snapshot().queue.empty(), "Downloads complete at bound locations");
        location_settings("source"); locations.persist();
        const auto tracked = locations.library();
        require(tracked.at("files").at(0).at("downloaded") && tracked.at("files").at(1).at("downloaded"), "History tracks both destinations independent of default");
        {
            WebApplication restarted(locations.root);
            require(restarted.settings_json().at("locationPreference") == "source" && restarted.settings_json().at("lastLocation") == "auto", "Routing and last selection survive restart");
            require(restarted.library().at("files").at(0).at("downloaded"), "Download history survives restart");
        }
        const auto first_path = queued[0].destination / std::filesystem::u8path(files[0].name).filename();
        auto unplugged = queued[0].destination; unplugged += "-unplugged";
        std::filesystem::rename(queued[0].destination, unplugged);
        require(locations.library().at("files").at(0).at("downloadState") == "Location unavailable", "Unavailable directory is distinguished");
        std::filesystem::rename(unplugged, queued[0].destination);
        require(locations.library().at("files").at(0).at("downloaded"), "A copy is recognized again when its location returns");
        std::filesystem::remove(first_path);
        require(locations.library().at("files").at(0).at("downloadState") == "Missing", "Deleted file is reported missing");
        locations.command({{"action", "deleteDownload"}, {"revision", locations.model.snapshot().catalog_revision}, {"index", 0}, {"path", first_path.u8string()}});
        const auto deleted = locations.library().at("files").at(0);
        require(deleted.at("downloadState") == "Not downloaded" && deleted.at("downloadCopies").empty(), "Deleting a missing copy removes only its record");
        {
            WebApplication restarted(locations.root);
            require(restarted.library().at("files").at(0).at("downloadCopies").empty(), "The removed record stays removed after restart");
        }
        std::ofstream(queued[1].destination / std::filesystem::u8path(files[1].name).filename(), std::ios::binary | std::ios::app) << "changed";
        require(locations.library().at("files").at(1).at("downloadState") == "Changed", "Modified downloads are not reported valid");
        const auto copy_directory = locations.root / "second-copy";
        std::filesystem::create_directories(copy_directory);
        std::ofstream(copy_directory / std::filesystem::u8path(files[0].name).filename(), std::ios::binary) << "abc";
        locations.record_download(files[0], copy_directory);
        require(locations.library().at("files").at(0).at("downloaded"), "Any surviving valid copy is recognized");
        std::filesystem::remove(copy_directory / std::filesystem::u8path(files[0].name).filename());
        auto second_unplugged = queued[1].destination; second_unplugged += "-unplugged";
        std::filesystem::rename(queued[1].destination, second_unplugged);
        require(locations.command({{"action", "cleanDownloads"}}).at("removed") == 1, "Clean removes only missing copies");
        const auto cleaned = locations.library().at("files");
        require(cleaned.at(0).at("downloadCopies").empty() && cleaned.at(1).at("downloadState") == "Location unavailable", "Clean keeps unavailable locations");
        require(locations.command({{"action", "cleanDownloads"}, {"unavailable", true}}).at("removed") == 1 && locations.library().at("files").at(1).at("downloadCopies").empty(),
            "Clean removes unavailable locations only when asked");
        std::filesystem::rename(second_unplugged, queued[1].destination);
        require(rejects({{"action", "enqueue"}, {"revision", locations.model.snapshot().catalog_revision}, {"indices", {0}}, {"locationRevision", locations.location_revision + 1}}), "Stale location preflight is rejected");
        locations.config.ca_bundle = "existing-ca.pem";
        location_settings("auto");
        require(locations.config.ca_bundle == "existing-ca.pem" && read_config(locations.settings, true).ca_bundle == "existing-ca.pem", "Saving settings without the removed CA field preserves existing configuration");
    }
    console_smoke(client, headers, application.root);
    merge_smoke(application.root);
    result = client.Post("/api/shutdown", headers, "{}", "application/json");
    require(result && result->status == 200, "Graceful HTTP shutdown response");
    std::cout << "Web API smoke passed: assets, origin/host/session guards, settings, secrecy, favorites, queue, overwrite, persistence" << std::endl;
}

}
