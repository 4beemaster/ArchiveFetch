#include "thread_stack.hpp"

#include <cstdlib>
#include <new>
#include <pthread.h>
#ifndef __linux__
#include <pthread_np.h>
#endif
#include <signal.h>
#include <thread>

// Linked with --wrap=pthread_create, so std::thread, std::async, httplib and curl threads all come through here.
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);

namespace {

constexpr size_t signal_stack_size = 64 * 1024;

struct Start {
    void* (*routine)(void*);
    void* argument;
};

// Each thread needs its own signal stack, or a stack overflow kills the process before the crash handler can log.
void* start_thread(void* opaque) {
    const Start start = *static_cast<Start*>(opaque);
    delete static_cast<Start*>(opaque);
    stack_t stack{};
    stack.ss_sp = std::malloc(signal_stack_size);
    stack.ss_size = signal_stack_size;
    if (stack.ss_sp && sigaltstack(&stack, nullptr) != 0) {
        std::free(stack.ss_sp);
        stack.ss_sp = nullptr;
    }
    void* result = start.routine(start.argument);
    if (stack.ss_sp) {
        stack_t disabled{};
        disabled.ss_flags = SS_DISABLE;
        sigaltstack(&disabled, nullptr);
        std::free(stack.ss_sp);
    }
    return result;
}

}

extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attributes, void* (*routine)(void*),
    void* argument) {
    pthread_attr_t sized;
    const bool own = !attributes && pthread_attr_init(&sized) == 0;
    if (own) pthread_attr_setstacksize(&sized, archivefetch_diag::thread_stack_size);
    auto* start = new (std::nothrow) Start{routine, argument};
    const int result = start ? __real_pthread_create(thread, own ? &sized : attributes, start_thread, start)
                             : __real_pthread_create(thread, own ? &sized : attributes, routine, argument);
    if (start && result != 0) delete start;
    if (own) pthread_attr_destroy(&sized);
    return result;
}

namespace archivefetch_diag {

long long measured_thread_stack() {
    long long measured = -1;
    try {
        std::thread([&measured] {
            pthread_attr_t attributes;
            size_t size = 0;
#ifdef __linux__
            if (pthread_getattr_np(pthread_self(), &attributes) != 0) return;
            if (pthread_attr_getstacksize(&attributes, &size) == 0) measured = static_cast<long long>(size);
#else
            if (pthread_attr_init(&attributes) != 0) return;
            if (pthread_attr_get_np(pthread_self(), &attributes) == 0 && pthread_attr_getstacksize(&attributes, &size) == 0)
                measured = static_cast<long long>(size);
#endif
            pthread_attr_destroy(&attributes);
        }).join();
    } catch (...) {}
    return measured;
}

}
