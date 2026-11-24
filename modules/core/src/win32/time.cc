#include "korvid/core/time.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace kv {

static u64 qpc_frequency() {
    static u64 f = 0;
    if (!f) {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        f = (u64)li.QuadPart;
    }
    return f;
}

u64 os_ticks() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (u64)li.QuadPart;
}

u64 os_ticks_per_second() { return qpc_frequency(); }

f64 time_now() { return (f64)os_ticks() / (f64)qpc_frequency(); }

// The thread's user and kernel time, kept by Windows in 100 ns units (and updated at the
// scheduler's tick, so it is coarse), in the profiler's ticks.
u64 os_thread_cpu_ticks() {
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
    u64 k = ((u64)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
    u64 u = ((u64)user.dwHighDateTime << 32) | user.dwLowDateTime;
    u64 hundreds = k + u;
    u64 f = qpc_frequency();
    return (hundreds / 10000000ull) * f + (hundreds % 10000000ull) * f / 10000000ull;
}

// A high-resolution waitable timer: Sleep() rounds up to the scheduler's tick, 15.6 ms
// unless something raised the timer resolution.
void time_sleep(f64 seconds) {
    if (seconds <= 0.0) return;
    static thread_local HANDLE timer = nullptr;
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(seconds * 1e7); // relative, in 100 ns units
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    Sleep((DWORD)(seconds * 1000.0));
}

} // namespace kv

namespace kv {

TimeDate time_local_date() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return TimeDate{st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond};
}

} // namespace kv
