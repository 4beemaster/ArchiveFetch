#include "payload_manager.hpp"
#include <iostream>
#include <mutex>
#include <thread>

namespace {
using Json = nlohmann::json;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    const auto root = std::filesystem::temp_directory_path() / ("archive-pm-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto elf = root / "ArchiveFetch.elf";
    const std::string bytes = std::string("\x7f" "ELF") + std::string(200000, 'P');
    std::ofstream(elf, std::ios::binary) << bytes;
    httplib::Server server;
    std::mutex mutex;
    Json listing, check;
    int upload_status = 200, writes = 0;
    std::string upload_body = "OK", uploaded, filename, mode;
    std::vector<std::string> calls;
    server.set_pre_routing_handler([&](const httplib::Request& request, httplib::Response& response) {
        std::lock_guard<std::mutex> lock(mutex);
        calls.push_back(request.method + " " + request.path);
        if (request.method == "POST" && request.path == "/manage:upload" &&
            request.get_header_value("X-PLDMGR-Request") != "1") {
            response.status = 403;
            response.set_content("Mutation origin or request marker rejected", "text/plain");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    server.Get("/version", [](const httplib::Request&, httplib::Response& response) { response.set_content("0.6.3", "text/plain"); });
    server.Get("/list_payloads", [&](const httplib::Request&, httplib::Response& response) {
        std::lock_guard<std::mutex> lock(mutex);
        response.set_content(listing.dump(), "application/json");
    });
    server.Get("/manage:check", [&](const httplib::Request&, httplib::Response& response) {
        std::lock_guard<std::mutex> lock(mutex);
        response.set_content(check.dump(), "application/json");
    });
    server.Post("/manage:upload", [&](const httplib::Request& request, httplib::Response& response) {
        std::lock_guard<std::mutex> lock(mutex);
        ++writes;
        filename = request.get_param_value("filename");
        mode = request.get_param_value("mode");
        uploaded = request.body;
        response.status = upload_status;
        response.set_content(upload_body, "text/plain");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    if (port <= 0) { std::cerr << "Cannot start stand-in Payload Manager\n"; return 1; }
    std::thread listener([&] { server.listen_after_bind(); });
    server.wait_until_ready();
    int exit_code = 0;
    try {
        httplib::Client client("127.0.0.1", port);
        const auto reset = [&](const Json& value, const std::string& folder = "ArchiveFetch") {
            std::lock_guard<std::mutex> lock(mutex);
            listing = value;
            check = {{"folder_name", folder}, {"folder_exists", true}, {"file_exists", true}};
            writes = 0; upload_status = 200; upload_body = "OK"; uploaded.clear(); calls.clear();
        };
        const auto legacy = Json({{"payloads", {"/data/pldmgr/payloads/ArchiveFetch/ArchiveFetch.elf", "/mnt/usb0/ArchiveFetch.elf"}}});
        reset(legacy);
        const auto unmarked = client.Post("/manage:upload?filename=ArchiveFetch.elf&mode=replace", "", "application/octet-stream");
        require(unmarked && unmarked->status == 403, "Payload Manager 0.6.3 rejects an upload without its request marker");
        const auto wrong_marker = client.Post("/manage:upload?filename=ArchiveFetch.elf&mode=replace",
            httplib::Headers{{"X-PLDMGR-Request", "0"}}, "", "application/octet-stream");
        require(wrong_marker && wrong_marker->status == 403, "The request marker must be exactly 1");
        { std::lock_guard<std::mutex> lock(mutex); require(writes == 0, "Rejected requests must not reach the upload handler"); }
        reset(legacy);
        auto result = archive_catalog::sync_payload_manager(client, elf);
        {
            std::lock_guard<std::mutex> lock(mutex);
            require(result.replaced && result.filename == "ArchiveFetch.elf" && writes == 1 && uploaded == bytes &&
                filename == "ArchiveFetch.elf" && mode == "replace", "0.5 replaces only the stored copy, with exact bytes and name");
            require(calls == std::vector<std::string>{"GET /version", "GET /list_payloads", "GET /manage:check", "POST /manage:upload", "GET /list_payloads"},
                "Sync must never launch, delete or change the boot list");
        }
        reset({{"items", {{{"path", "/data/pldmgr/payloads/local/ArchiveFetch/ArchiveFetch.elf"}, {"slot", "local/ArchiveFetch"}}}}});
        result = archive_catalog::sync_payload_manager(client, elf);
        require(result.replaced && result.filename == "ArchiveFetch.elf", "0.6 replaces the existing local slot");
        reset({{"payloads", {"/data/pldmgr/payloads/ArchiveFetch/ArchiveFetch-0.7.2.elf"}}});
        result = archive_catalog::sync_payload_manager(client, elf);
        require(result.replaced && result.filename == "ArchiveFetch-0.7.2.elf", "Keep a versioned stored filename unchanged");
        reset({{"payloads", {"/mnt/usb0/ArchiveFetch.elf", "/data/pldmgr/payloads/Other/Other.elf"}}});
        require(!archive_catalog::sync_payload_manager(client, elf).replaced, "Do not create a missing copy or replace a USB copy");
        { std::lock_guard<std::mutex> lock(mutex); require(writes == 0, "Absent ArchiveFetch must not cause an upload"); }
        const auto rejected = [&](const Json& value, const char* message, const std::string& folder = "ArchiveFetch") {
            reset(value, folder);
            bool failed = false;
            try { archive_catalog::sync_payload_manager(client, elf); }
            catch (const std::exception&) { failed = true; }
            std::lock_guard<std::mutex> lock(mutex);
            require(failed && writes == 0, message);
        };
        rejected({{"items", {{{"path", "/data/pldmgr/payloads/repo/ArchiveFetch/ArchiveFetch.elf"}, {"slot", "repo/ArchiveFetch"}}}}},
            "Do not overwrite a repository-managed payload");
        rejected({{"payloads", {"/data/pldmgr/payloads/ArchiveFetch/ArchiveFetch.elf", "/data/pldmgr/payloads/Old/ArchiveFetch-0.7.1.elf"}}},
            "Multiple stored copies require an explicit choice");
        rejected({{"payloads", {"/data/pldmgr/payloads/ArchiveFetch/ArchiveFetch.elf", "/data/pldmgr/payloads/ArchiveFetch/Other.elf"}}},
            "Do not overwrite a folder shared with another payload");
        rejected(legacy, "A destination mismatch must not write", "Other");
        rejected({{"payloads", {"/data/pldmgr/payloads/Other/ArchiveFetch.elf"}}},
            "Never upload into another payload's folder even when its destination check matches", "Other");
        rejected(Json::object(), "Malformed list must not be treated as an absent copy");
        rejected({{"items", "invalid"}}, "Malformed modern list must not be ignored");
        rejected({{"payloads", {"/data/pldmgr/payloads/../ArchiveFetch/ArchiveFetch.elf"}}},
            "Unsafe stored paths must not be written");
        for (const int status : {500, 200}) {
            reset(legacy);
            {
                std::lock_guard<std::mutex> lock(mutex);
                upload_status = status;
                upload_body = R"({"success":false,"error":"write failed"})";
            }
            bool failed = false;
            try { archive_catalog::sync_payload_manager(client, elf); }
            catch (const std::exception&) { failed = true; }
            require(failed, "Both HTTP and JSON upload errors must be reported");
        }
        std::cout << "All Payload Manager sync tests passed\n";
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        exit_code = 1;
    }
    server.stop();
    listener.join();
    std::filesystem::remove(elf);
    std::filesystem::remove(root);
    return exit_code;
}
