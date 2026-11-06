#include "instance_handoff.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <thread>

#include "diag_log.hpp"
#include "http_service.hpp"

namespace archivefetch_diag {
namespace {

using Json = nlohmann::json;

// Covers an earlier copy finishing AppInstallAll (about 16 s) before it releases the port.
constexpr int wait_limit_ms = 40000;
constexpr int poll_interval_ms = 500;

bool is_archivefetch_diag(const char* body) {
    // Diag builds report archivefetch-diag, and 0.2.0-diag.8 predates the service field.
    return std::strstr(body, "\"service\":\"archivefetch\"") != nullptr ||
        std::strstr(body, "\"service\":\"archivefetch-diag\"") != nullptr ||
        std::strstr(body, "\"version\":\"0.2.0-diag.") != nullptr;
}

void extract_version(const char* body, char* out, size_t size) {
    out[0] = '\0';
    const char* start = std::strstr(body, "\"version\":\"");
    if (!start) return;
    start += std::strlen("\"version\":\"");
    const char* end = std::strchr(start, '"');
    if (!end) return;
    std::snprintf(out, size, "%.*s", static_cast<int>(end - start), start);
}

bool wait_for_port(int port) {
    for (int waited_ms = 0; waited_ms < wait_limit_ms; waited_ms += poll_interval_ms) {
        if (http_port_free(port)) {
            log_event("TAKEOVER_PORT_FREE", {field("waited_ms", waited_ms)});
            return true;
        }
        if (waited_ms > 0 && waited_ms % 5000 == 0)
            log_event("TAKEOVER_WAIT", {field("waited_ms", waited_ms), field("limit_ms", wait_limit_ms)});
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
    }
    return http_port_free(port);
}

}

const char* handoff_outcome_name(HandoffOutcome outcome) {
    switch (outcome) {
    case HandoffOutcome::PortFreed: return "port-freed";
    case HandoffOutcome::NotOurs: return "not-ours";
    case HandoffOutcome::StillHeld: return "still-held";
    }
    return "unknown";
}

HandoffResult handoff_previous_instance(int port) {
    HandoffResult result{HandoffOutcome::StillHeld, "", {}, {}};
    const LoopbackResponse status = http_loopback_request(port, "GET", "/status");
    const bool ours = status.status == 200 && is_archivefetch_diag(status.body);
    if (ours) extract_version(status.body, result.previous_version, sizeof(result.previous_version));
    log_event("TAKEOVER_STATUS", {field("port", port), field("connected", status.connected ? "true" : "false"),
        field("status", status.status), field("ours", ours ? "true" : "false"),
        field("previous_version", result.previous_version), field("errno", status.error)});

    if (status.connected && status.status != 0 && !ours) {
        result.outcome = HandoffOutcome::NotOurs;
    } else {
        if (ours) {
            LoopbackResponse stop = http_loopback_request(port, "POST", "/shutdown", "X-ArchiveFetch-Handoff: keys\r\n");
            const Json keys = stop.status == 200 ? Json::parse(stop.body, nullptr, false) : Json();
            if (keys.is_object() && keys.contains("access") && keys.contains("secret") && keys.at("access").is_string() && keys.at("secret").is_string()) {
                result.access_key = keys.at("access").get<std::string>();
                result.secret_key = keys.at("secret").get<std::string>();
            }
            OPENSSL_cleanse(stop.body, sizeof(stop.body));
            log_event("TAKEOVER_SHUTDOWN_SENT", {field("connected", stop.connected ? "true" : "false"),
                field("status", stop.status), field("errno", stop.error),
                field("keys_received", result.access_key.empty() ? "false" : "true")});
        }
        // No answer at all usually means an earlier copy is already stopping, so wait either way.
        result.outcome = wait_for_port(port) ? HandoffOutcome::PortFreed : HandoffOutcome::StillHeld;
    }
    log_event("TAKEOVER_END", {field("outcome", handoff_outcome_name(result.outcome)),
        field("previous_version", result.previous_version)});
    return result;
}

}
