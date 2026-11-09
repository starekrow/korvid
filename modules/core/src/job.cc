#include "korvid/core/job.h"
#include "korvid/core/atomic.h"
#include "korvid/core/arena.h"
#include "korvid/core/array.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/profile.h"
#include "korvid/core/thread.h"
#include "korvid/core/os_thread.h"
#include <stdio.h>

namespace kv {

namespace {

struct Task {
    JobBatch* batch;
    u32       chunk;
};

struct JobPool {
    OsMutex         mu;
    OsCond          cv;        // broadcast when tasks arrive, when one finishes, and on quit
    Array<Task>     queue;     // FIFO from head
    u32             head = 0;
    OsThread*       threads = nullptr;
    u32             workers = 0;
    bool            quit    = false;
    bool            up      = false;
};

JobPool g_pool;
JobStats g_stats;
thread_local u32 t_depth = 0; // batches this thread is waiting on, nested

void bump(u64* counter) { kv::atomic_add_fetch(counter, 1, kv::MO_RELAXED); }

void run_task(const Task& t) {
    JobBatch& b = *t.batch;
    u32 begin = t.chunk * b.grain;
    u32 end   = begin + b.grain < b.count ? begin + b.grain : b.count;
    u16 was = mem_owner();
    mem_set_owner(b.owner);
    {
        KV_ZONE(b.name);
        b.fn(t.chunk, begin, end, b.user);
    }
    mem_set_owner(was);
    kv::atomic_add_fetch(&b.done, 1, kv::MO_ACQ_REL);
}

// Under the lock: a task if there is one.
bool pop(Task* out) {
    JobPool& p = g_pool;
    if (p.head >= p.queue.len) return false;
    *out = p.queue[p.head++];
    if (p.head == p.queue.len) { p.queue.clear(); p.head = 0; }
    return true;
}

void worker_main(void* arg) {
    JobPool& p = g_pool;
    thread_id();
#if KV_PROFILE
    char name[32];
    snprintf(name, sizeof name, "worker %u", (u32)(usize)arg);
    prof_thread_name(name);
#else
    (void)arg;
#endif
    for (;;) {
        Task t;
        os_mutex_lock(&p.mu);
        while (!p.quit && !pop(&t)) {
            bump(&g_stats.worker_waits);
            KV_ZONE_WAIT("idle");
            os_cond_wait(&p.cv, &p.mu);
        }
        if (p.quit) { os_mutex_unlock(&p.mu); break; }
        os_mutex_unlock(&p.mu);
        if (t_depth == 0) mem_scratch_reset(); // job lifetime
        bump(&g_stats.tasks);
        run_task(t);
        os_mutex_lock(&p.mu);
        os_cond_broadcast(&p.cv); // someone may be waiting on this batch
        os_mutex_unlock(&p.mu);
    }
    mem_thread_exit();
}

} // namespace

bool jobs_init(u32 workers) {
    JobPool& p = g_pool;
    if (p.up && p.workers == workers) return true;
    if (p.up) jobs_shutdown();
    os_mutex_init(&p.mu);
    os_cond_init(&p.cv);
    p.queue.alloc = mem_general();
    p.quit = false;
    p.workers = 0;
    p.up = true;
    if (workers) {
        p.threads = alloc_items<OsThread>(mem_general(), workers);
        for (u32 i = 0; i < workers; ++i) {
            p.threads[i] = OsThread();
            if (!os_thread_start(&p.threads[i], worker_main, (void*)(usize)(i + 1))) {
                KV_ERROR("jobs", "could not start worker %u of %u", i + 1, workers);
                jobs_shutdown();
                return false;
            }
            ++p.workers;
        }
    }
    KV_INFO("jobs", "%u worker%s", p.workers, p.workers == 1 ? "" : "s");
    return true;
}

void jobs_shutdown() {
    JobPool& p = g_pool;
    if (!p.up) return;
    os_mutex_lock(&p.mu);
    p.quit = true;
    os_cond_broadcast(&p.cv);
    os_mutex_unlock(&p.mu);
    for (u32 i = 0; i < p.workers; ++i) os_thread_join(&p.threads[i]);
    if (p.threads) free_items(mem_general(), p.threads, p.workers ? p.workers : 1);
    p.threads = nullptr;
    p.workers = 0;
    p.queue.free();
    p.head = 0;
    os_mutex_destroy(&p.mu);
    os_cond_destroy(&p.cv);
    p.up = false;
}

u32 jobs_worker_count() { return g_pool.up ? g_pool.workers : 0; }

JobStats jobs_stats() {
    JobStats s;
    s.batches        = kv::atomic_load(&g_stats.batches, kv::MO_RELAXED);
    s.inline_batches = kv::atomic_load(&g_stats.inline_batches, kv::MO_RELAXED);
    s.tasks          = kv::atomic_load(&g_stats.tasks, kv::MO_RELAXED);
    s.tasks_by_caller = kv::atomic_load(&g_stats.tasks_by_caller, kv::MO_RELAXED);
    s.helper_waits   = kv::atomic_load(&g_stats.helper_waits, kv::MO_RELAXED);
    s.worker_waits   = kv::atomic_load(&g_stats.worker_waits, kv::MO_RELAXED);
    return s;
}

void jobs_stats_reset() { g_stats = JobStats{}; }

void jobs_run(u32 count, u32 grain, JobFn fn, void* user, const char* name) {
    u32 chunks = jobs_chunks(count, grain);
    if (chunks == 0) return;
    JobBatch b{fn, user, count, grain, chunks, mem_owner(), 0, name};
    JobPool& p = g_pool;
    if (!p.up || p.workers == 0 || chunks == 1) {
        bump(&g_stats.inline_batches);
        for (u32 i = 0; i < chunks; ++i) run_task(Task{&b, i});
        return;
    }
    bump(&g_stats.batches);
    os_mutex_lock(&p.mu);
    for (u32 i = 0; i < chunks; ++i) p.queue.push(Task{&b, i});
    os_cond_broadcast(&p.cv);
    os_mutex_unlock(&p.mu);

    // Help until this batch is done: any task counts, including another batch's.
    ++t_depth;
    for (;;) {
        Task t;
        os_mutex_lock(&p.mu);
        bool got = pop(&t);
        if (!got) {
            if (kv::atomic_load(&b.done, kv::MO_ACQUIRE) == chunks) { os_mutex_unlock(&p.mu); break; }
            bump(&g_stats.helper_waits);
            {
                KV_ZONE_WAIT("batch wait");
                os_cond_wait(&p.cv, &p.mu);
            }
            os_mutex_unlock(&p.mu);
            continue;
        }
        os_mutex_unlock(&p.mu);
        bump(&g_stats.tasks);
        bump(&g_stats.tasks_by_caller);
        run_task(t);
        os_mutex_lock(&p.mu);
        os_cond_broadcast(&p.cv);
        os_mutex_unlock(&p.mu);
    }
    --t_depth;
}

} // namespace kv
