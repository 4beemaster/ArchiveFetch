#pragma once

#include <cstddef>

namespace archivefetch_diag {

// The console's default is 64 KiB; ps5upload sizes its threads the same way.
inline constexpr std::size_t thread_stack_size = 512 * 1024;

// Stack size of a freshly started std::thread, or -1 if it cannot be read.
long long measured_thread_stack();

}
