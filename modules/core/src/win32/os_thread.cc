#include "korvid/core/os_thread.h"
#include "korvid/core/atomic.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/thread.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <avrt.h> // MMCSS; avrt.lib, the toolchain's external "avrt"

namespace kv {

static_assert(sizeof(SRWLOCK) <= sizeof(OsMutex::storage), "OsMutex is too small for an SRWLOCK");
static_assert(sizeof(CONDITION_VARIABLE) <= sizeof(OsCond::storage), "OsCond is too small for a CONDITION_VARIABLE");
static_assert(sizeof(HANDLE) <= sizeof(u64), "a HANDLE does not fit OsThread::handle");

static SRWLOCK*            native(OsMutex* m) { return reinterpret_cast<SRWLOCK*>(m->storage); }
static CONDITION_VARIABLE* native(OsCond* c)  { return reinterpret_cast<CONDITION_VARIABLE*>(c->storage); }

u32 os_cpu_count() {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 ? (u32)n : 1;
}

// A slim reader/writer lock, held exclusively: no kernel object, nothing to destroy.
void os_mutex_init(OsMutex* m)    { InitializeSRWLock(native(m)); }
void os_mutex_destroy(OsMutex*)   {}
void os_mutex_lock(OsMutex* m)    { AcquireSRWLockExclusive(native(m)); }
void os_mutex_unlock(OsMutex* m)  { ReleaseSRWLockExclusive(native(m)); }

void os_cond_init(OsCond* c)             { InitializeConditionVariable(native(c)); }
void os_cond_destroy(OsCond*)            {}
void os_cond_wait(OsCond* c, OsMutex* m) { SleepConditionVariableSRW(native(c), native(m), INFINITE, 0); }
// Milliseconds rounded up, so a wait is never shorter than asked; INFINITE is one past
// the longest (49 days).
bool os_cond_timed_wait(OsCond* c, OsMutex* m, f64 seconds) {
    if (!(seconds > 0)) return false;
    f64 ms = seconds * 1000.0;
    DWORD wait = ms >= 4294967294.0 ? 4294967294u : (DWORD)ms;
    if ((f64)wait < ms && wait < 4294967294u) ++wait;
    if (SleepConditionVariableSRW(native(c), native(m), wait, 0)) return true;
    return GetLastError() != ERROR_TIMEOUT;
}
void os_cond_signal(OsCond* c)           { WakeConditionVariable(native(c)); }
void os_cond_broadcast(OsCond* c)        { WakeAllConditionVariable(native(c)); }

static DWORD WINAPI thread_start(LPVOID arg) {
    OsThread* t = (OsThread*)arg;
    t->fn(t->arg);
    return 0;
}

bool os_thread_start(OsThread* t, OsThreadFn fn, void* arg) {
    t->fn = fn;
    t->arg = arg;
    // The stack is reserved, not committed: pages are committed as the thread uses them.
    HANDLE h = CreateThread(nullptr, 8u << 20, thread_start, t, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) return false;
    t->handle = (u64)(usize)h;
    return true;
}

void os_thread_join(OsThread* t) {
    HANDLE h = (HANDLE)(usize)t->handle;
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    t->handle = 0;
}

// The Multimedia Class Scheduler's "Pro Audio" task: the thread is raised while it runs,
// and the service keeps a share of each core for the rest. The task stays the thread's
// until it ends.
static u32 g_audio_refusal_logged = 0; // atomic

bool os_thread_enter_audio_class() {
    DWORD task = 0;
    HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    if (!h && atomic_exchange(&g_audio_refusal_logged, 1u, MO_RELAXED) == 0)
        KV_WARN("thread", "realtime audio class refused: AvSetMmThreadCharacteristicsW(\"Pro Audio\"): error %lu", (unsigned long)GetLastError());
    thread_set_realtime(true);
    return h != nullptr;
}

} // namespace kv
