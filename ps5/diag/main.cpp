#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <thread>

#include "diag_config.hpp"
#include "api_routes.hpp"
#include "diag_identity.hpp"
#include "diag_log.hpp"
#include "http_service.hpp"
#include "instance_handoff.hpp"
#include "media_shortcut.hpp"
#include "native_libraries.hpp"
#include "thread_stack.hpp"
#include "web_assets.hpp"

#include <ps5/kernel.h>

using namespace archivefetch_diag;

static void announce(const char* message) {
    const NotifyResult outcome = notify(message);
    log_event("NOTIFICATION_END", {field("available", outcome.available ? "true" : "false"),
        field_hex("result", outcome.result), field("message", message)});
}

static int finish(int code) {
    log_event("EXIT", {field("code", code)});
    log_close();
    return code;
}

static void register_tile() {
    log_event("THREAD_NAME_END", {field("role", "media-register"), field("requested", process_name),
        field_hex("result", name_thread())});
    const InstallerExports exports = resolve_installer();
    const bool registered = stage_shortcut() && register_shortcut(exports);
    char message[160];
    std::snprintf(message, sizeof(message), registered ? "%s tile registered" : "%s tile registration failed",
        version);
    announce(message);
}

int main() {
    int open_error = 0;
    // After a factory reset neither folder exists, and mkdir does not create parents.
    mkdir(app_data_root, 0777);
    mkdir(log_directory, 0777);
    const long long rotated = log_rotate(log_path, log_previous_path, log_rotate_bytes);
    if (!log_open(log_directory, log_path, &open_error)) {
        log_event("LOG_OPEN_FAILED", {field("path", log_path), field("errno", open_error),
            field("error", std::strerror(open_error))});
        char message[160];
        std::snprintf(message, sizeof(message), "%s cannot open log: %s", version, std::strerror(open_error));
        notify(message);
        return 1;
    }

    log_event("ENTRY", {field("log_path", log_path),
        field_hex("firmware", static_cast<int>(kernel_get_fw_version()))});
    if (rotated != 0)
        log_event("LOG_ROTATED", {field("previous_path", log_previous_path), field("previous_bytes", rotated)});
    log_event("PROCESS_NAME_END", {field("requested", process_name), field_hex("result", name_process())});
    log_event("THREAD_NAME_END", {field("role", "main"), field("requested", process_name),
        field_hex("result", name_thread())});
    // A browser closing mid-response must not terminate the process.
    std::signal(SIGPIPE, SIG_IGN);
    log_install_crash_handler();
    log_event("THREAD_STACK", {field("requested", static_cast<long long>(thread_stack_size)),
        field("measured", measured_thread_stack())});
    log_event("WEB_ASSETS", {field("count", static_cast<long long>(web_asset_count())),
        field("bytes", static_cast<long long>(web_asset_bytes()))});

    char message[160];
    std::snprintf(message, sizeof(message), "%s started", version);
    announce(message);

    // Before the handoff, so a hang here leaves the previous copy serving.
    if (!init_native_libraries()) {
        std::snprintf(message, sizeof(message), "%s native library setup failed; see log", version);
        announce(message);
    }

    HttpStart started = http_start(http_port);
    std::string handed_access, handed_secret;
    if (started == HttpStart::PortInUse) {
        HandoffResult handoff = handoff_previous_instance(http_port);
        handed_access.swap(handoff.access_key);
        handed_secret.swap(handoff.secret_key);
        if (handoff.outcome == HandoffOutcome::PortFreed) {
            if (handoff.previous_version[0]) {
                std::snprintf(message, sizeof(message), "%s replacing %s", version, handoff.previous_version);
                announce(message);
            }
            started = http_start(http_port);
        } else {
            std::snprintf(message, sizeof(message), handoff.outcome == HandoffOutcome::NotOurs ?
                "%s: port %d is used by another program" : "%s: previous copy on port %d did not stop",
                version, http_port);
            announce(message);
            return finish(1);
        }
    }
    if (started != HttpStart::Listening) {
        std::snprintf(message, sizeof(message), "%s cannot listen on port %d", version, http_port);
        announce(message);
        return finish(1);
    }
    if (!http_self_test(http_port)) {
        std::snprintf(message, sizeof(message), "%s HTTP self-test failed on port %d", version, http_port);
        announce(message);
        http_stop();
        http_close();
        return finish(1);
    }

    std::thread registration;
    try {
        registration = std::thread(register_tile);
    } catch (const std::system_error& failure) {
        log_event("MEDIA_THREAD_FAILED", {field("error", failure.what())});
    }

    // Only after the port is ours, so a previous copy has already saved its data.
    const bool app_ready = api_init(app_data_root);
    if (!handed_access.empty()) api_adopt_keys(handed_access, handed_secret);
    std::snprintf(message, sizeof(message), app_ready ? "%s ready on port %d" :
        "%s on port %d, but ArchiveFetch data failed to load; see log", version, http_port);
    announce(message);
    log_event("HTTP_SERVING", {field("port", http_port), field("app", app_ready ? "ready" : "failed"),
        field("registration_thread", registration.joinable() ? "started" : "failed")});
    http_wait();

    // Saved before the port is released, so a successor loads current data.
    api_shutdown();

    // Never exit while AppInstallAll may still be running.
    if (registration.joinable()) {
        log_event("MEDIA_THREAD_JOIN_BEGIN");
        registration.join();
        log_event("MEDIA_THREAD_JOIN_END");
    }
    // Holding the port until now keeps a successor from starting its own AppInstallAll alongside ours.
    http_close();
    return finish(0);
}
