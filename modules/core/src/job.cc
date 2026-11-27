#include "korvid/core/job.h"
#include "korvid/core/format.h"
#include "korvid/core/atomic.h"
#include "korvid/core/arena.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/profile.h"
#include "korvid/core/thread.h"
#include "korvid/core/time.h"
#include "korvid/core/os_thread.h"

// How the pool is built, and why (numbers: examples/cpukiller, tools/concurrency-numbers.sh).
//
// A batch is queued once, whole, and lives on its caller's stack. A thread takes a chunk
// with one atomic add on the batch's `claim`; the pool's lock is taken once for each batch
// a thread joins, to find it and to leave it, never for a chunk. The first pool queued
// every chunk and took the lock twice a chunk, with a broadcast to every sleeper after
// each: 180 ns a chunk where a bare claim is 14, and slower on sixteen threads than on four
// once chunks fell under a microsecond.
//
// What keeps a batch on the stack safe: a thread is inside a batch only between joining it
// and leaving it, both under the lock, and is counted in `refs` meanwhile. The caller
// returns only when it has seen `refs` at zero under the lock with every chunk claimed, so
// no thread is in the batch or can find it again.
//
// A thread with nothing to do spins for a while (jobs_set_idle_spin) before it sleeps, watching one word
// (`epoch`, bumped when a batch is queued). A tick is several batches a few tens of
// microseconds apart; a worker that slept between them would be woken by the OS for each,
// which costs more than the gap.

namespace kv {

namespace {

// How long an idle thread watches for work before it sleeps, and a caller for its last
// chunks before it does. Long enough to bridge the serial stretch between one system's
// batch and the next; short enough that a pool with no work is asleep within a frame's
// rounding error.
constinit f64 g_spin_seconds = 0.0001; // jobs_set_idle_spin

struct Batch {
    JobFn       fn;
    void*       user;
    u32         count, grain, chunks;
    u16         owner;      // mem owner (module index + 1) for attribution inside the chunks
    const char* name;       // the profiler's name for the chunks
    // Under the pool's lock:
    Batch*      prev = nullptr;
    Batch*      next = nullptr;
    u32         refs = 0;        // atomic: threads besides the caller inside the batch; written under the lock
    bool        waiting = false; // the caller is asleep on cv
    bool        cv_made = false;
    OsCond      cv{};            // the caller's, made only if it has to sleep
    // The one word every thread writes, on a cache line of its own (padded by hand: cl
    // warns of alignas on a member).
    u8          pad0[64] = {};
    u32         claim = 0;       // atomic: the next chunk to take
    u8          pad1[60] = {};
};

struct JobPool {
    OsMutex         mu{};
    OsCond          cv{};      // idle workers; signalled when a batch arrives, and on quit
    Batch*          head = nullptr; // batches in flight, oldest first
    Batch*          tail = nullptr;
    u32             sleepers = 0;   // workers in os_cond_wait on cv
    u32             epoch = 0;      // atomic: bumped under the lock when a batch arrives or the pool quits
    OsThread*       threads = nullptr;
    u32             workers = 0;
    bool            quit    = false;
    bool            up      = false;
};

constinit JobPool g_pool;
constinit JobStats g_stats{};

void bump(u64* counter, u64 by = 1) { if (by) kv::atomic_add_fetch(counter, by, kv::MO_RELAXED); }

void run_chunk(const Batch& b, u32 chunk) {
    u32 begin = chunk * b.grain;
    u32 end   = begin + b.grain < b.count ? begin + b.grain : b.count;
    u16 was = mem_owner();
    mem_set_owner(b.owner);
    {
        KV_ZONE(b.name);
        b.fn(chunk, begin, end, b.user);
    }
    mem_set_owner(was);
}

// Takes chunks until there are none to take; returns how many it ran. A worker resets its
// scratch before each (job lifetime); a caller's scratch is its own chunk's, or its frame's.
u32 run_chunks(Batch& b, bool worker) {
    u32 chunks = b.chunks, ran = 0;
    // The load first: a thread that finds the batch claimed out does not write the line.
    while (kv::atomic_load(&b.claim, kv::MO_RELAXED) < chunks) {
        u32 c = kv::atomic_fetch_add(&b.claim, 1, kv::MO_RELAXED);
        if (c >= chunks) break;
        if (worker) mem_scratch_reset();
        run_chunk(b, c);
        ++ran;
    }
    return ran;
}

// Under the lock: the oldest batch with a chunk unclaimed, other than `skip`.
Batch* find_open(const Batch* skip) {
    for (Batch* b = g_pool.head; b; b = b->next)
        if (b != skip && kv::atomic_load(&b->claim, kv::MO_RELAXED) < b->chunks) return b;
    return nullptr;
}

// Under the lock: into a batch, and out of it. The last thread out wakes a caller asleep
// on it; after the unlock that follows, this thread must not touch the batch again.
void join(Batch* b) { kv::atomic_add_fetch(&b->refs, 1, kv::MO_RELAXED); }
void leave(Batch* b) {
    if (kv::atomic_sub_fetch(&b->refs, 1, kv::MO_RELEASE) == 0 && b->waiting) os_cond_signal(&b->cv);
}

// Spins until the pool's epoch moves from `seen` or the idle spin runs out. True if it moved.
bool spin_for_work(u32 seen) {
    JobPool& p = g_pool;
    if (g_spin_seconds <= 0.0) return false;
    u64 until = os_ticks() + (u64)(g_spin_seconds * (f64)os_ticks_per_second());
    for (;;) {
        for (u32 i = 0; i < 16; ++i) {
            if (kv::atomic_load(&p.epoch, kv::MO_RELAXED) != seen) return true;
            atomic_pause();
        }
        if (os_ticks() >= until) return false;
    }
}

void worker_main(void* arg) {
    JobPool& p = g_pool;
    thread_id();
#if KV_PROFILE
    char name[32];
    format(name, sizeof name, "worker %u", (u32)(usize)arg);
    prof_thread_name(name);
#else
    (void)arg;
#endif
    os_mutex_lock(&p.mu);
    while (!p.quit) {
        Batch* b = find_open(nullptr);
        if (!b) {
            bump(&g_stats.worker_waits);
            KV_ZONE_WAIT("idle");
            if (g_spin_seconds > 0.0) {
                u32 seen = kv::atomic_load(&p.epoch, kv::MO_RELAXED);
                os_mutex_unlock(&p.mu);
                bool moved = spin_for_work(seen);
                os_mutex_lock(&p.mu);
                // A batch queued while the lock was let go moved the epoch: look again, do not sleep.
                if (moved || kv::atomic_load(&p.epoch, kv::MO_RELAXED) != seen) continue;
            }
            ++p.sleepers;
            os_cond_wait(&p.cv, &p.mu);
            --p.sleepers;
            continue;
        }
        join(b);
        os_mutex_unlock(&p.mu);
        bump(&g_stats.tasks, run_chunks(*b, true));
        os_mutex_lock(&p.mu);
        leave(b);
    }
    os_mutex_unlock(&p.mu);
    mem_thread_exit();
}

void run_inline(const Batch& b) {
    bump(&g_stats.inline_batches);
    for (u32 i = 0; i < b.chunks; ++i) run_chunk(b, i);
}

} // namespace

bool jobs_init(u32 workers) {
    JobPool& p = g_pool;
    if (p.up && p.workers == workers) return true;
    if (p.up) jobs_shutdown();
    os_mutex_init(&p.mu);
    os_cond_init(&p.cv);
    p.head = p.tail = nullptr;
    p.sleepers = 0;
    p.quit = false;
    p.workers = 0;
    p.up = true;
    if (workers) {
        p.threads = alloc_items<OsThread>(mem_general(), workers);
        if (!p.threads) { // a heap at its limit: the pool runs every batch on the caller
            KV_ERROR("jobs", "no memory for %u workers; running without", workers);
            KV_INFO("jobs", "0 workers");
            return true;
        }
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
    kv::atomic_add_fetch(&p.epoch, 1, kv::MO_RELAXED);
    os_cond_broadcast(&p.cv);
    os_mutex_unlock(&p.mu);
    for (u32 i = 0; i < p.workers; ++i) os_thread_join(&p.threads[i]);
    if (p.threads) free_items(mem_general(), p.threads, p.workers ? p.workers : 1);
    p.threads = nullptr;
    p.workers = 0;
    os_mutex_destroy(&p.mu);
    os_cond_destroy(&p.cv);
    p.up = false;
}

u32 jobs_worker_count() { return g_pool.up ? g_pool.workers : 0; }

void jobs_set_idle_spin(f64 seconds) { g_spin_seconds = seconds > 0.0 ? seconds : 0.0; }
f64  jobs_idle_spin() { return g_spin_seconds; }

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

// Each counter stored on its own: a worker that found nothing after the last batch counts its
// wait as it goes to sleep, which is after jobs_run has returned to the caller.
void jobs_stats_reset() {
    u64* const all[] = {&g_stats.batches, &g_stats.inline_batches, &g_stats.tasks, &g_stats.tasks_by_caller,
                        &g_stats.helper_waits, &g_stats.worker_waits};
    for (u64* c : all) kv::atomic_store(c, u64{0}, kv::MO_RELAXED);
}

void jobs_run(u32 count, u32 grain, JobFn fn, void* user, const char* name) {
    u32 chunks = jobs_chunks(count, grain);
    if (chunks == 0) return;
    Batch b{fn, user, count, grain, chunks, mem_owner(), name};
    JobPool& p = g_pool;
    if (!p.up || p.workers == 0 || chunks == 1) { run_inline(b); return; }

    // Queue it, and wake as many hands as it has chunks for besides this one: idle workers
    // first, then callers asleep on batches of their own, who will help with this one.
    os_mutex_lock(&p.mu);
    b.prev = p.tail;
    if (p.tail) p.tail->next = &b; else p.head = &b;
    p.tail = &b;
    kv::atomic_add_fetch(&p.epoch, 1, kv::MO_RELAXED);
    u32 want = chunks - 1 < p.workers ? chunks - 1 : p.workers;
    if (want >= p.sleepers) { if (p.sleepers) os_cond_broadcast(&p.cv); want -= p.sleepers; }
    else { for (u32 i = 0; i < want; ++i) os_cond_signal(&p.cv); want = 0; }
    for (Batch* o = p.head; o && want; o = o->next)
        if (o->waiting) { os_cond_signal(&o->cv); --want; }
    os_mutex_unlock(&p.mu);
    bump(&g_stats.batches);

    u32 ran = run_chunks(b, false);

    // Every chunk is claimed; some may still be running on other threads. Until they are
    // out, work on any other batch that has chunks to take, so a batch started inside a
    // chunk never waits on a thread that is itself waiting; sleep only when there is nothing.
    if (g_spin_seconds > 0.0) {
        u64 until = os_ticks() + (u64)(g_spin_seconds * (f64)os_ticks_per_second());
        while (kv::atomic_load(&b.refs, kv::MO_ACQUIRE) != 0 && os_ticks() < until)
            for (u32 i = 0; i < 16; ++i) atomic_pause();
    }
    os_mutex_lock(&p.mu);
    while (kv::atomic_load(&b.refs, kv::MO_ACQUIRE) != 0) {
        if (Batch* o = find_open(&b)) {
            join(o);
            os_mutex_unlock(&p.mu);
            ran += run_chunks(*o, false);
            os_mutex_lock(&p.mu);
            leave(o);
            continue;
        }
        bump(&g_stats.helper_waits);
        if (!b.cv_made) { os_cond_init(&b.cv); b.cv_made = true; }
        b.waiting = true;
        {
            KV_ZONE_WAIT("batch wait");
            os_cond_wait(&b.cv, &p.mu);
        }
        b.waiting = false;
    }
    if (b.prev) b.prev->next = b.next; else p.head = b.next;
    if (b.next) b.next->prev = b.prev; else p.tail = b.prev;
    os_mutex_unlock(&p.mu);
    if (b.cv_made) os_cond_destroy(&b.cv);
    bump(&g_stats.tasks, ran);
    bump(&g_stats.tasks_by_caller, ran);
}

} // namespace kv
