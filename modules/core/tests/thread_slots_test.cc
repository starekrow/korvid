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
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif
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
struct AudioClass { bool granted = false, marked = false, constrained = false; int policy = -1; };
void enter_audio_class(void* arg) {
    AudioClass& a = *(AudioClass*)arg;
    a.granted = os_thread_enter_audio_class();
    a.marked = thread_is_realtime();
#if defined(__linux__) && !defined(__ANDROID__)
    a.policy = sched_getscheduler(0) & ~SCHED_RESET_ON_FORK;
#elif defined(__APPLE__) && TARGET_OS_OSX
    // What the kernel holds for the thread. is_default goes both ways: in, true asks for
    // the default policy instead; out, true says the thread has none of its own.
    thread_time_constraint_policy_data_t got{};
    mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t is_default = 0;
    a.constrained = thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                                      (thread_policy_t)&got, &count, &is_default) == KERN_SUCCESS &&
                    !is_default && got.period != 0;
#endif
    thread_set_realtime(false); // the thread's exit gives its memory slot back
}

} // namespace

// os_thread_enter_audio_class marks the thread for the allocators whatever the OS says;
// on Linux the thread is SCHED_FIFO exactly when it says yes, and with RLIMIT_RTPRIO at 0
// (a desktop session's) it says no unless the process may ignore the limit. macOS grants
// the time constraint policy to any process, and the kernel holds it afterwards.
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
#elif defined(__APPLE__) && TARGET_OS_OSX
        KV_CHECK(c, a.granted && a.constrained);
#endif
    }
    KV_CHECK(c, !thread_is_realtime()); // the caller's thread is untouched
}

namespace {
struct Waited { kv::OsMutex m; kv::OsCond cv; bool ready = false; };
void signal_later(void* arg) {
    Waited* w = (Waited*)arg;
    kv::time_sleep(0.01);
    kv::os_mutex_lock(&w->m);
    w->ready = true;
    kv::os_cond_signal(&w->cv);
    kv::os_mutex_unlock(&w->m);
}
} // namespace

void test_cond_timed_wait(kv::TestCtx& c) {
    using namespace kv;
    c.begin("cond timed wait");
    Waited w;
    os_mutex_init(&w.m);
    os_cond_init(&w.cv);
    os_mutex_lock(&w.m);
    // No time: false at once, the mutex still held (unlocking it below would fail otherwise).
    f64 t0 = time_now();
    KV_CHECK(c, !os_cond_timed_wait(&w.cv, &w.m, 0.0) && !os_cond_timed_wait(&w.cv, &w.m, -1.0));
    KV_CHECK(c, time_now() - t0 < 0.005);
    // Nothing signals: the time runs out, and not before it.
    t0 = time_now();
    bool woke = os_cond_timed_wait(&w.cv, &w.m, 0.02);
    f64 waited = time_now() - t0;
    // A spurious wake is allowed to say true; one that does so early is not a timeout.
    KV_CHECK(c, woke ? waited < 0.02 : waited >= 0.0195);
    KV_CHECK(c, waited < 1.0);
    // A signal before the deadline: true, long before it.
    OsThread t;
    KV_CHECK(c, os_thread_start(&t, signal_later, &w));
    t0 = time_now();
    bool signalled = true;
    while (!w.ready && signalled) signalled = os_cond_timed_wait(&w.cv, &w.m, 5.0);
    KV_CHECK(c, w.ready && signalled && time_now() - t0 < 4.0);
    os_mutex_unlock(&w.m);
    os_thread_join(&t);
    os_cond_destroy(&w.cv);
    os_mutex_destroy(&w.m);
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
