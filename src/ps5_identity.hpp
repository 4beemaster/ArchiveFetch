#pragma once

#if defined(PS5_PAYLOAD) && defined(ARCHIVEFETCH_PS5_IDENTITY)
#include <dlfcn.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace archive_catalog {

#ifndef ARCHIVEFETCH_THREAD_NAME
#define ARCHIVEFETCH_THREAD_NAME "ArchiveFetch"
#endif

inline int name_archivefetch_process(int (*setter)(const char*)) noexcept {
    return setter ? setter("ArchiveFetch.elf") : -1;
}

inline int name_archivefetch_process() noexcept {
#if defined(PS5_PAYLOAD) && defined(ARCHIVEFETCH_PS5_IDENTITY)
    const auto setter = reinterpret_cast<int (*)(const char*)>(dlsym(RTLD_DEFAULT, "sceKernelSetProcessName"));
    return name_archivefetch_process(setter);
#else
    return 0;
#endif
}

inline int name_archivefetch_thread() noexcept {
#if defined(PS5_PAYLOAD) && defined(ARCHIVEFETCH_PS5_IDENTITY)
#ifdef SYS_thr_set_name
    return static_cast<int>(syscall(SYS_thr_set_name, -1, ARCHIVEFETCH_THREAD_NAME));
#else
    return -1;
#endif
#else
    return 0;
#endif
}

}