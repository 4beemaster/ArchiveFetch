#include "web_application.hpp"
#include "console_link.hpp"
#ifndef __ANDROID__
#include "web_smoke.hpp"
#endif

namespace {

#ifdef _WIN32
class NavigationGuard final : public ICoreWebView2NavigationStartingEventHandler, public ICoreWebView2NewWindowRequestedEventHandler {
public:
    explicit NavigationGuard(const std::string& origin) : origin_(origin.begin(), origin.end()) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (IsEqualIID(id, IID_IUnknown) || IsEqualIID(id, IID_ICoreWebView2NavigationStartingEventHandler)) *object = static_cast<ICoreWebView2NavigationStartingEventHandler*>(this);
        else if (IsEqualIID(id, IID_ICoreWebView2NewWindowRequestedEventHandler)) *object = static_cast<ICoreWebView2NewWindowRequestedEventHandler*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* arguments) override {
        LPWSTR uri = nullptr;
        if (FAILED(arguments->get_Uri(&uri))) { arguments->put_Cancel(TRUE); return S_OK; }
        std::wstring target(uri); CoTaskMemFree(uri);
        if (target != origin_ && target.rfind(origin_ + L"/", 0) != 0) arguments->put_Cancel(TRUE);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* arguments) override { return arguments->put_Handled(TRUE); }
private:
    std::atomic<ULONG> references_{1};
    std::wstring origin_;
};

void secure_window(webview::webview& window, const std::string& origin) {
    auto* controller = static_cast<ICoreWebView2Controller*>(window.browser_controller().value());
    ICoreWebView2* browser = nullptr;
    if (FAILED(controller->get_CoreWebView2(&browser))) throw std::runtime_error("Cannot initialize WebView2");
    auto* guard = new NavigationGuard(origin);
    EventRegistrationToken navigation{}, new_window{};
    auto navigation_result = browser->add_NavigationStarting(guard, &navigation);
    auto window_result = browser->add_NewWindowRequested(guard, &new_window);
    guard->Release();
    ICoreWebView2Settings* settings = nullptr;
    if (SUCCEEDED(browser->get_Settings(&settings))) {
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->Release();
    }
    browser->Release();
    if (FAILED(navigation_result) || FAILED(window_result)) throw std::runtime_error("Cannot secure WebView2 navigation");
}
#endif

}

#if !defined(__ANDROID__) && !defined(ARCHIVEFETCH_PS5_HOST)
int main(int argc, char** argv) {
    bool native_error = true;
    try {
        bool browser = true;
        bool smoke = false;
        std::filesystem::path isolated;
        int port = 0;
        for (int index = 1; index < argc; ++index) {
            std::string argument = argv[index];
            if (argument == "--no-browser") browser = false;
            else if (argument == "--smoke-test") { smoke = true; browser = false; }
            else if (argument == "--data-dir" && index + 1 < argc) isolated = std::filesystem::u8path(argv[++index]);
            else if (argument == "--port" && index + 1 < argc) port = std::stoi(argv[++index]);
            else throw std::runtime_error("Usage: archive_catalog_web [--no-browser] [--port number] [--data-dir path] [--smoke-test]");
        }
        if (smoke && isolated.empty()) isolated = std::filesystem::temp_directory_path() / ("archive-web-" + random_token().substr(0, 12));
        native_error = browser;
        if (smoke && std::filesystem::exists(isolated) && !std::filesystem::is_empty(isolated)) throw std::runtime_error("Smoke tests require an empty data directory");
        DiagnosticLog diagnostic_log;
        WebApplication application(isolated);
        if (smoke) {
            require(application.model.snapshot().sources.empty(), "Fresh installation has no bundled sources");
            Config fixture = application.config; fixture.sources = {"https://archive.org/details/archive-web-test"}; fixture.follow_parts = false;
            application.model.start_scan(fixture, [](const std::string&) { return "<files><file name='Alpha.chd'><size>3</size><sha1>a9993e364706816aba3e25717850c26c9cd0d89d</sha1></file><file name='Beta.chd' private='true'><size>3</size><md5>900150983cd24fb0d6963f7d28e17f72</md5></file></files>"; });
            application.model.wait();
            application.model.restore_sources({{"https://archive.org/details/archive-web-test", "Test collection", true, {}, {}}});
            application.config.download_dir = application.root / "downloads";
            std::filesystem::create_directories(application.config.download_dir);
            std::ofstream(application.config.download_dir / "Alpha.chd", std::ios::binary) << "abc";
        }
        httplib::Server server;
        server.new_task_queue = [] { return new httplib::ThreadPool(1); };
        server.set_keep_alive_max_count(1);
        server.set_payload_max_length(1024 * 1024);
        server.set_read_timeout(10, 0);
        server.set_write_timeout(10, 0);
        auto session = random_token();
        int bound = port ? (server.bind_to_port("127.0.0.1", port) ? port : -1) : server.bind_to_any_port("127.0.0.1");
        if (bound < 0) throw std::runtime_error("Cannot bind local web interface");
        auto host = "127.0.0.1:" + std::to_string(bound);
        auto origin = "http://" + host;
        const auto allowed = [&](const httplib::Request& request) {
            if (request.get_header_value("Host") != host || (request.has_header("Origin") && request.get_header_value("Origin") != origin) ||
                request.get_header_value("Sec-Fetch-Site") == "cross-site") return false;
            return request.path.rfind("/api/", 0) != 0 || request.get_header_value("X-Archive-Session") == session;
        };
        // Requests with a body are refused in their handler instead: closing before reading the body can reset the connection before the 403 arrives.
        const auto guarded = [&](httplib::Server::Handler handler) {
            return [&allowed, handler](const httplib::Request& request, httplib::Response& response) {
                if (!allowed(request)) { response.status = 403; return; }
                handler(request, response);
            };
        };
        server.set_pre_routing_handler([&](const httplib::Request& request, httplib::Response& response) {
            response.set_header("Cache-Control", "no-store");
            response.set_header("X-Content-Type-Options", "nosniff");
            response.set_header("Referrer-Policy", "no-referrer");
            response.set_header("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; font-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
            if (!allowed(request) && (request.method != "POST" || request.get_header_value_u64("Content-Length") == 0)) {
                response.status = 403;
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        server.Get("/session", [&](const httplib::Request&, httplib::Response& response) {
            response.set_content(Json({{"token", session}, {"version", ARCHIVEFETCH_VERSION}, {"consoles", true}}).dump(), "application/json");
        });
        server.Get("/api/state", [&](const httplib::Request&, httplib::Response& response) { response.set_content(application.snapshot().dump(), "application/json"); });
        server.Get("/api/library", [&](const httplib::Request&, httplib::Response& response) { response.set_content(application.library().dump(), "application/json"); });
        server.Get("/api/diagnostics", [&](const httplib::Request&, httplib::Response& response) { response.set_content(Json({{"log", diagnostic_log.text()}}).dump(), "application/json"); });
        server.Post("/api/diagnostics/clear", guarded([&](const httplib::Request&, httplib::Response& response) { diagnostic_log.clear(); response.set_content("{}", "application/json"); }));
        server.Post("/api/command", guarded([&](const httplib::Request& request, httplib::Response& response) {
            try {
                if (request.get_header_value("Content-Type") != "application/json") throw std::runtime_error("Expected JSON request");
                response.set_content(application.command(Json::parse(request.body)).dump(), "application/json");
            } catch (const std::exception& failure) {
                response.status = 400;
                response.set_content(Json({{"error", failure.what()}}).dump(), "application/json");
            }
        }));
        server.Post("/api/shutdown", guarded([&](const httplib::Request&, httplib::Response& response) { response.set_content("{}", "application/json"); }));
        ConsoleLink consoles(application.root / "consoles.json", platform_console_secrets());
        consoles.payload_dir = executable_directory(argv[0]) / "payload";
        auto console_handler = [&](const char* route, bool list) {
            return [&consoles, route, list](const httplib::Request& request, httplib::Response& response) {
                try {
                    Json body = {{"refresh", request.has_param("refresh")}};
                    if (!list) {
                        if (request.get_header_value("Content-Type") != "application/json") throw std::runtime_error("Expected JSON request");
                        body = Json::parse(request.body);
                    }
                    int status = 200;
                    const auto result = console_route(consoles, route, body, status);
                    response.status = status;
                    response.set_content(result.dump(), "application/json");
                } catch (const std::exception& failure) {
                    response.status = 400;
                    response.set_content(Json({{"error", failure.what()}}).dump(), "application/json");
                }
            };
        };
        server.Get("/api/consoles", console_handler("/api/consoles", true));
        server.Post("/api/consoles", guarded(console_handler("/api/consoles", false)));
        server.Post("/api/relay", guarded(console_handler("/api/relay", false)));
        server.set_logger([&](const httplib::Request& request, const httplib::Response& response) {
            if (request.method == "POST" && request.path == "/api/shutdown" && response.status == 200) { application.model.cancel(); server.stop(); }
        });
        auto assets = executable_directory(argv[0]) / "web";
        if (!server.set_mount_point("/", assets.u8string())) throw std::runtime_error("Bundled web assets are missing");
        std::cout << origin << std::endl;
        if (smoke) {
            std::thread listener([&] { server.listen_after_bind(); });
            server.wait_until_ready();
            try { web_smoke(server, application, bound, session); }
            catch (...) { server.stop(); listener.join(); throw; }
            listener.join(); return 0;
        }
#ifdef _WIN32
        if (browser) {
            std::thread listener([&] { server.listen_after_bind(); });
            server.wait_until_ready();
            try {
                webview::webview window(false, nullptr);
                secure_window(window, origin);
                window.set_title("ArchiveFetch");
                window.set_size(1360, 860, WEBVIEW_HINT_NONE);
                window.set_size(760, 520, WEBVIEW_HINT_MIN);
                window.navigate(origin);
                window.run();
            } catch (...) { server.stop(); listener.join(); throw; }
            server.stop(); listener.join();
            return 0;
        }
#endif
        server.listen_after_bind();
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
#ifdef _WIN32
        if (native_error) MessageBoxA(nullptr, failure.what(), "ArchiveFetch", MB_OK | MB_ICONERROR);
#endif
        return 1;
    }
}
#elif defined(__ANDROID__)
#include "android_bridge.hpp"
#endif
