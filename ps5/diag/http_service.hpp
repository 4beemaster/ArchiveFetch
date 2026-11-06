#pragma once

namespace archivefetch_diag {

enum class HttpStart { Listening, PortInUse, Failed };

// Binds 0.0.0.0:port and serves from one named listener thread.
HttpStart http_start(int port);

// Requests /status over loopback and checks that this process answered.
bool http_self_test(int port);

struct LoopbackResponse {
    bool connected;
    int error;
    int status;  // 0 when no HTTP status line arrived
    char body[2560];
};

// One request to 127.0.0.1:port with a 5 s timeout; body is truncated to fit. extra_headers lines end in \r\n.
LoopbackResponse http_loopback_request(int port, const char* method, const char* path, const char* extra_headers = "");

// True when the port can be bound right now; the test socket is closed again.
bool http_port_free(int port);

// Blocks until a loopback POST /shutdown is served or http_stop() is called; the port stays held.
void http_wait();

void http_stop();

// Releases the port; call only after any work a successor must not overlap has finished.
void http_close();

}
