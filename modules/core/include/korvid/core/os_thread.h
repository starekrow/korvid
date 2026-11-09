// Korvid core: the thread layer. Threads, mutexes and condition variables from the OS, in
// fixed-size storage so that no header includes the platform's own. Implemented per
// platform under src/<platform>/os_thread.cc. A thread gets the same 8 MiB stack on every
// platform, Linux's default, so code that runs on one runs on all.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// Room for any platform's native object (pthread_mutex_t: 40 bytes on Linux, 64 on
// macOS; an SRWLOCK: 8); each implementation checks its own fits.
struct OsMutex { alignas(8) u8 storage[64]; };
struct OsCond  { alignas(8) u8 storage[64]; };

void os_mutex_init(OsMutex* m);
void os_mutex_destroy(OsMutex* m);
void os_mutex_lock(OsMutex* m);
void os_mutex_unlock(OsMutex* m);

void os_cond_init(OsCond* c);
void os_cond_destroy(OsCond* c);
// Releases m, waits for a signal or broadcast (or wakes spuriously), and retakes m.
void os_cond_wait(OsCond* c, OsMutex* m);
void os_cond_signal(OsCond* c);
void os_cond_broadcast(OsCond* c);

typedef void (*OsThreadFn)(void* arg);

// The thread reads fn and arg from here when it starts, so the OsThread must stay where
// it is until os_thread_join.
struct OsThread {
    u64        handle = 0;
    OsThreadFn fn     = nullptr;
    void*      arg    = nullptr;
};

bool os_thread_start(OsThread* t, OsThreadFn fn, void* arg);
void os_thread_join(OsThread* t);

} // namespace kv
