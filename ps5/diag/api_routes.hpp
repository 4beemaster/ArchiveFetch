#pragma once

#include <cstddef>
#include <string>

namespace httplib {
class Server;
struct Request;
struct Response;
}

namespace archivefetch_diag {

// The largest request body: a PS5 payload sent to /api/update. Every other route is limited to small_body_limit.
inline constexpr std::size_t update_body_limit = 64u << 20;
inline constexpr std::size_t small_body_limit = 1u << 20;

// Loads ArchiveFetch data from data_root; returns false (and API routes answer 503) if it cannot start.
bool api_init(const char* data_root);

// Registers /session and /api/*; called for every server the listener builds.
void api_register_routes(httplib::Server& http);

// Refuses peers outside the local network unless external connections are turned on.
bool api_accept_peer(const httplib::Request& request, httplib::Response& response);

// Stops running work and saves the library; API routes answer 503 afterwards.
void api_shutdown();

// JSON with the in-memory S3 keys for a successor, only when they are not saved to disk; otherwise a plain stop message.
std::string api_handoff_keys();

// Uses keys handed over by the previous copy unless saved keys were loaded; wipes the given strings.
void api_adopt_keys(std::string& access_key, std::string& secret_key);

}
