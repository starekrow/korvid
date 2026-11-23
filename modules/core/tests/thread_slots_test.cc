// The memory system's per-thread accounting slots: 256 of them, one per live thread. A
// thread that exits gives its slot back, so after 300 threads have come and gone a new
// one is still counted where reports look while it runs, and its totals fold on exit.
#include "korvid/core/atomic.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_thread.h"
#include "korvid/core/test.h"
#include "korvid/core/thread.h"
#include "korvid/core/time.h"
#if defined(__linux__) && !defined(__ANDROID__)
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace {

using namespace kv;

void churn(void*) { mem_free(mem_alloc(64)); }

struct Holder {
    u32   held = 0, release = 0;
    void* blocks[50] = {};
};

void hold(void* arg) {
    Holder& h = *(Holder*)arg;
    for (void*& b : h.blocks) b = mem_alloc(32);
    atomic_store(&h.held, 1u, MO_RELEASE);
    while (!atomic_load(&h.release, MO_ACQUIRE)) time_sleep(0.001);
    for (void* b : h.blocks) mem_free(b);
}

// A thread of the program's own asks for the audio class, and says what it got.
struct AudioClass { bool granted = false, marked = false; int policy = -1; };
void enter_audio_class(void* arg) {
    AudioClass& a = *(AudioClass*)arg;
    a.granted = os_thread_enter_audio_class();
    a.marked = thread_is_realtime();
#if defined(__linux__) && !defined(__ANDROID__)
    a.policy = sched_getscheduler(0) & ~SCHED_RESET_ON_FORK;
#endif
    thread_set_realtime(false); // the thread's exit gives its memory slot back
}

} // namespace

// os_thread_enter_audio_class marks the thread for the allocators whatever the OS says;
// on Linux the thread is SCHED_FIFO exactly when it says yes, and with RLIMIT_RTPRIO at 0
// (a desktop session's) it says no unless the process may ignore the limit.
void test_thread_audio_class(kv::TestCtx& c) {
    using namespace kv;
    c.begin("thread audio class");
    for (u32 i = 0; i < 2; ++i) { // twice: the second refusal is not logged again
        AudioClass a;
        OsThread t;
        KV_CHECK(c, os_thread_start(&t, enter_audio_class, &a));
        os_thread_join(&t);
        KV_CHECK(c, a.marked);
#if defined(__linux__) && !defined(__ANDROID__)
        KV_CHECK(c, a.granted == (a.policy == SCHED_FIFO));
        struct rlimit rl{};
        getrlimit(RLIMIT_RTPRIO, &rl);
        if (rl.rlim_cur == 0 && geteuid() != 0) KV_CHECK(c, !a.granted);
        KV_INFO("test", "audio class: %s (RLIMIT_RTPRIO %lld)", a.granted ? "granted, SCHED_FIFO" : "refused",
                rl.rlim_cur == RLIM_INFINITY ? -1ll : (long long)rl.rlim_cur);
#endif
    }
    KV_CHECK(c, !thread_is_realtime()); // the caller's thread is untouched
}

void test_thread_slots(kv::TestCtx& c) {
    using namespace kv;
    c.begin("thread slots");
    for (u32 i = 0; i < 300; ++i) { // more threads than slots, one after another
        OsThread t;
        KV_CHECK(c, os_thread_start(&t, churn, nullptr));
        os_thread_join(&t);
    }
    u64 base = mem_live_allocations();
    Holder h;
    OsThread t;
    KV_CHECK(c, os_thread_start(&t, hold, &h));
    while (!atomic_load(&h.held, MO_ACQUIRE)) time_sleep(0.001);
    KV_CHECK(c, mem_live_allocations() == base + 50); // seen while the thread runs
    atomic_store(&h.release, 1u, MO_RELEASE);
    os_thread_join(&t);
    KV_CHECK(c, mem_live_allocations() == base);
}
