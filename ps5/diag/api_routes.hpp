#pragma once

#include <string>

namespace httplib {
class Server;
struct Request;
struct Response;
}

namespace archivefetch_diag {

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
