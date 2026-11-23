#include "korvid/core/os_thread.h"
#include "korvid/core/atomic.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/thread.h"
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#if defined(__linux__) && !defined(__ANDROID__)
#include <sched.h>
#include <sys/resource.h>
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif

namespace kv {

static_assert(sizeof(pthread_mutex_t) <= sizeof(OsMutex::storage), "OsMutex is too small for pthread_mutex_t");
static_assert(sizeof(pthread_cond_t) <= sizeof(OsCond::storage), "OsCond is too small for pthread_cond_t");
static_assert(sizeof(pthread_t) <= sizeof(u64), "pthread_t does not fit OsThread::handle");

static pthread_mutex_t* native(OsMutex* m) { return reinterpret_cast<pthread_mutex_t*>(m->storage); }
static pthread_cond_t*  native(OsCond* c)  { return reinterpret_cast<pthread_cond_t*>(c->storage); }

u32 os_cpu_count() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (u32)n : 1;
}

void os_mutex_init(OsMutex* m)    { pthread_mutex_init(native(m), nullptr); }
void os_mutex_destroy(OsMutex* m) { pthread_mutex_destroy(native(m)); }
void os_mutex_lock(OsMutex* m)    { pthread_mutex_lock(native(m)); }
void os_mutex_unlock(OsMutex* m)  { pthread_mutex_unlock(native(m)); }

void os_cond_init(OsCond* c)                { pthread_cond_init(native(c), nullptr); }
void os_cond_destroy(OsCond* c)             { pthread_cond_destroy(native(c)); }
void os_cond_wait(OsCond* c, OsMutex* m)    { pthread_cond_wait(native(c), native(m)); }
void os_cond_signal(OsCond* c)              { pthread_cond_signal(native(c)); }
void os_cond_broadcast(OsCond* c)           { pthread_cond_broadcast(native(c)); }

static void* thread_start(void* arg) {
    OsThread* t = (OsThread*)arg;
    t->fn(t->arg);
    return nullptr;
}

bool os_thread_start(OsThread* t, OsThreadFn fn, void* arg) {
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_t tid;
    int rc = pthread_create(&tid, &attr, thread_start, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) return false;
    t->handle = 0;
    memcpy(&t->handle, &tid, sizeof tid);
    return true;
}

void os_thread_join(OsThread* t) {
    pthread_t tid;
    memcpy(&tid, &t->handle, sizeof tid);
    pthread_join(tid, nullptr);
    t->handle = 0;
}

// ---- the audio class --------------------------------------------------------------------

static u32 g_audio_refusal_logged = 0; // atomic: the first refusal logs, the rest are quiet

// The OS's no, logged once a process. Before the thread is marked: logging may allocate.
static void audio_class_refused(const char* why) {
    if (atomic_exchange(&g_audio_refusal_logged, 1u, MO_RELAXED) == 0) KV_WARN("thread", "realtime audio class refused: %s", why);
}

bool os_thread_enter_audio_class() {
    bool granted = false;
#if defined(__linux__) && !defined(__ANDROID__)
    // Inside the limit an unprivileged process may ask for; a process with CAP_SYS_NICE
    // (root) is not held to it, so a limit of 0 is still asked, at the usual priority.
    constexpr int kPriority = 80;
    struct rlimit rl{};
    getrlimit(RLIMIT_RTPRIO, &rl);
    int prio = kPriority;
    if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur > 0 && (rlim_t)prio > rl.rlim_cur) prio = (int)rl.rlim_cur;
    int most = sched_get_priority_max(SCHED_FIFO);
    if (most > 0 && prio > most) prio = most;
    sched_param sp{};
    sp.sched_priority = prio;
    if (sched_setscheduler(0, SCHED_FIFO | SCHED_RESET_ON_FORK, &sp) == 0) granted = true;
    else {
        int err = errno;
        char why[200];
        if (err == EPERM && rl.rlim_cur == 0)
            format(why, sizeof why, "RLIMIT_RTPRIO is 0 (ulimit -r; a desktop session grants realtime through rtkit, which core does not ask)");
        else
            format(why, sizeof why, "SCHED_FIFO at %d: %s (RLIMIT_RTPRIO %lld)", prio, strerror(err),
                     rl.rlim_cur == RLIM_INFINITY ? -1ll : (long long)rl.rlim_cur);
        audio_class_refused(why);
    }
#elif defined(__APPLE__) && TARGET_OS_OSX
    // The time constraint policy: of each period, the computation it needs and the
    // constraint it must finish within, in Mach absolute time. 128 frames at 44.1 kHz is
    // 2.9 ms; Core Audio's own threads ask for the like.
    mach_timebase_info_data_t tb{};
    mach_timebase_info(&tb);
    double per_ms = tb.numer ? 1e6 * (double)tb.denom / (double)tb.numer : 0.0;
    thread_time_constraint_policy_data_t policy{};
    policy.period      = (uint32_t)(2.9 * per_ms);
    policy.computation = (uint32_t)(0.75 * 2.9 * per_ms);
    policy.constraint  = (uint32_t)(0.85 * 2.9 * per_ms);
    policy.preemptible = 1;
    kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                                         (thread_policy_t)&policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr == KERN_SUCCESS) granted = true;
    else {
        char why[96];
        format(why, sizeof why, "THREAD_TIME_CONSTRAINT_POLICY: %s", mach_error_string(kr));
        audio_class_refused(why);
    }
#else
    audio_class_refused("the platform's audio thread is the one in the class (iOS, Android)");
#endif
    thread_set_realtime(true);
    return granted;
}

} // namespace kv
