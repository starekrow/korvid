// Korvid core: the thread layer: threads, mutexes and condition variables from the OS.
//
// The job pool's workers and its lock are built on these; game code reaches threads
// through the pool (job.h, and parallel_for in phase.h). Each object lives in
// fixed-size storage so that no header includes the platform's own, and is initialized
// and destroyed by hand: pthreads on POSIX, SRW locks and condition variables on Windows,
// under src/<platform>/os_thread.cc. A thread gets the same 8 MiB stack on every platform,
// Linux's default, so code that runs on one runs on all.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// ---- mutexes and condition variables --------------------------------------------------
// Room for any platform's native object (pthread_mutex_t: 40 bytes on Linux, 64 on
// macOS; an SRWLOCK: 8); each implementation checks its own fits. Initialized in place,
// a mutex or condition variable must not be moved or copied until it is destroyed.
struct OsMutex { alignas(8) u8 storage[64]; };
struct OsCond  { alignas(8) u8 storage[64]; };

// A mutex: initialize before first use, destroy after last. Not recursive: a thread must
// not lock a mutex it already holds.
void os_mutex_init(OsMutex* m);
void os_mutex_destroy(OsMutex* m);
void os_mutex_lock(OsMutex* m);
void os_mutex_unlock(OsMutex* m);

// A condition variable, used with a mutex: initialize before first use, destroy after
// last.
void os_cond_init(OsCond* c);
void os_cond_destroy(OsCond* c);
// Releases m, waits for a signal or broadcast (or wakes spuriously), and retakes m. The
// caller holds m, and checks its condition again in a loop around the wait.
void os_cond_wait(OsCond* c, OsMutex* m);
// The same with a deadline `seconds` from now: false when the time ran out first, true
// for any wake before it (a spurious one too, so the caller's loop is the same). 0 or less
// returns false at once, m still held. The deadline is on the monotonic clock: a change
// to the wall clock moves it nowhere.
bool os_cond_timed_wait(OsCond* c, OsMutex* m, f64 seconds);
// Wakes one waiter, or every waiter.
void os_cond_signal(OsCond* c);
void os_cond_broadcast(OsCond* c);

// ---- events ---------------------------------------------------------------------------
// A flag that one thread waits on and any thread sets. It resets itself: a set wakes the
// wait under way, or the next one to begin, and that wait takes it. Its timed wait keeps
// to the time asked for to well under a millisecond on every OS: on Windows a
// high-resolution waitable timer, where a condition variable's wait rounds up to the
// scheduler's tick (15.6 ms unless something raised the timer resolution). For a loop
// that sleeps a few milliseconds at a time and must wake at once when told. Initialized in
// place: not moved or copied until destroyed.
struct OsEvent { alignas(8) u8 storage[160]; };
void os_event_init(OsEvent* ev);
void os_event_destroy(OsEvent* ev);
// Sets it. Any thread; not a signal handler.
void os_event_set(OsEvent* ev);
// Waits until it is set or `seconds` pass, whichever is first: true if it was set (it is
// clear again now). 0 or less does not wait, and takes a set that is already there. One
// thread waits at a time.
bool os_event_wait(OsEvent* ev, f64 seconds);

// ---- threads --------------------------------------------------------------------------
// A thread's body: runs on the new thread with the `arg` given to os_thread_start; the
// thread ends when it returns.
typedef void (*OsThreadFn)(void* arg);

// The thread reads fn and arg from here when it starts, so the OsThread must stay where
// it is until os_thread_join.
struct OsThread {
    u64        handle = 0;       // the platform's thread handle; 0 before start and after join
    OsThreadFn fn     = nullptr; // set by os_thread_start
    void*      arg    = nullptr; // set by os_thread_start
};

// Starts a thread running fn(arg); false when the OS cannot create it.
bool os_thread_start(OsThread* t, OsThreadFn fn, void* arg);
// Waits for the thread to return and releases it. Once for each thread started.
void os_thread_join(OsThread* t);

// Asks the OS to put the calling thread in its class for realtime audio, for a thread of
// the program's own that fills a device's buffer (a thread the audio device owns, as
// platform_audio's backends' are, arrives in the class already). It marks the thread
// real-time for the allocators (thread_set_realtime(true), thread.h) whether or not the OS
// agrees, and returns whether it did; the first refusal in the process logs its reason.
//
// Linux: SCHED_FIFO with SCHED_RESET_ON_FORK, at priority 80 or the RLIMIT_RTPRIO limit
// if that is lower. A desktop session's limit is usually 0, and its realtime comes from
// rtkit over D-Bus, which core does not speak: refused, with `ulimit -r` named. Windows:
// MMCSS's "Pro Audio" task (AvSetMmThreadCharacteristicsW, avrt). macOS: the time
// constraint policy, for a period of 128 frames at 44.1 kHz. iOS and Android: false,
// since the platform's audio thread is the one in the class.
bool os_thread_enter_audio_class();

} // namespace kv
