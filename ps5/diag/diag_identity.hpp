#pragma once

#include "diag_config.hpp"

#if defined(PS5_PAYLOAD)
#include <dlfcn.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace archivefetch_diag {

inline int name_process() noexcept {
#if defined(PS5_PAYLOAD)
    using Setter = int (*)(const char*);
    const auto setter = reinterpret_cast<Setter>(dlsym(RTLD_DEFAULT, "sceKernelSetProcessName"));
    return setter ? setter(process_name) : -1;
#else
    return 0;
#endif
}

inline int name_thread() noexcept {
#if defined(PS5_PAYLOAD) && defined(SYS_thr_set_name)
    return static_cast<int>(syscall(SYS_thr_set_name, -1, process_name));
#else
    return 0;
#endif
}

}
