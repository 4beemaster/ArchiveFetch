#pragma once

// Every translation unit must see httplib with the same configuration.
#include <sys/socket.h>
// The PS5 libkernel has no accept4; without SOCK_CLOEXEC httplib falls back to accept.
#undef SOCK_CLOEXEC
#define CPPHTTPLIB_USE_POLL
#include <httplib.h>
