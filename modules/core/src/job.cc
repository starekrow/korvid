#include "korvid/core/job.h"
#include "korvid/core/arena.h"
#include "korvid/core/array.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/thread.h"
#include <pthread.h>

namespace kv {

namespace {

struct Task {
    JobBatch* batch;
    u32       chunk;
};

struct Pool {
    pthread_mutex_t mu;
    pthread_cond_t  cv;        // broadcast when tasks arrive, when one finishes, and on quit
    Array<Task>     queue;     // FIFO from head
    u32             head = 0;
    pthread_t*      threads = nullptr;
    u32             workers = 0;
    bool            quit    = false;
    bool            up      = false;
};

Pool g_pool;
thread_local u32 t_depth = 0; // batches this thread is waiting on, nested

void run_task(const Task& t) {
    JobBatch& b = *t.batch;
    u32 begin = t.chunk * b.grain;
    u32 end   = begin + b.grain < b.count ? begin + b.grain : b.count;
    u16 was = mem_owner();
    mem_set_owner(b.owner);
    b.fn(t.chunk, begin, end, b.user);
    mem_set_owner(was);
    __atomic_add_fetch(&b.done, 1, __ATOMIC_ACQ_REL);
}

// Under the lock: a task if there is one.
bool pop(Task* out) {
    Pool& p = g_pool;
    if (p.head >= p.queue.len) return false;
    *out = p.queue[p.head++];
    if (p.head == p.queue.len) { p.queue.clear(); p.head = 0; }
    return true;
}

void* worker_main(void*) {
    Pool& p = g_pool;
    thread_id();
    for (;;) {
        Task t;
        pthread_mutex_lock(&p.mu);
        while (!p.quit && !pop(&t)) pthread_cond_wait(&p.cv, &p.mu);
        if (p.quit) { pthread_mutex_unlock(&p.mu); break; }
        pthread_mutex_unlock(&p.mu);
        if (t_depth == 0) mem_scratch_reset(); // job lifetime
        run_task(t);
        pthread_mutex_lock(&p.mu);
        pthread_cond_broadcast(&p.cv); // someone may be waiting on this batch
        pthread_mutex_unlock(&p.mu);
    }
    mem_thread_exit();
    return nullptr;
}

} // namespace

bool jobs_init(u32 workers) {
    Pool& p = g_pool;
    if (p.up && p.workers == workers) return true;
    if (p.up) jobs_shutdown();
    pthread_mutex_init(&p.mu, nullptr);
    pthread_cond_init(&p.cv, nullptr);
    p.queue.alloc = mem_general();
    p.quit = false;
    p.workers = 0;
    p.up = true;
    if (workers) {
        p.threads = alloc_items<pthread_t>(mem_general(), workers);
        for (u32 i = 0; i < workers; ++i) {
            if (pthread_create(&p.threads[i], nullptr, worker_main, nullptr) != 0) {
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
    Pool& p = g_pool;
    if (!p.up) return;
    pthread_mutex_lock(&p.mu);
    p.quit = true;
    pthread_cond_broadcast(&p.cv);
    pthread_mutex_unlock(&p.mu);
    for (u32 i = 0; i < p.workers; ++i) pthread_join(p.threads[i], nullptr);
    if (p.threads) free_items(mem_general(), p.threads, p.workers ? p.workers : 1);
    p.threads = nullptr;
    p.workers = 0;
    p.queue.free();
    p.head = 0;
    pthread_mutex_destroy(&p.mu);
    pthread_cond_destroy(&p.cv);
    p.up = false;
}

u32 jobs_worker_count() { return g_pool.up ? g_pool.workers : 0; }


void jobs_run(u32 count, u32 grain, JobFn fn, void* user) {
    u32 chunks = jobs_chunks(count, grain);
    if (chunks == 0) return;
    JobBatch b{fn, user, count, grain, chunks, mem_owner(), 0};
    Pool& p = g_pool;
    if (!p.up || p.workers == 0 || chunks == 1) {
        for (u32 i = 0; i < chunks; ++i) run_task(Task{&b, i});
        return;
    }
    pthread_mutex_lock(&p.mu);
    for (u32 i = 0; i < chunks; ++i) p.queue.push(Task{&b, i});
    pthread_cond_broadcast(&p.cv);
    pthread_mutex_unlock(&p.mu);

    // Help until this batch is done: any task counts, including another batch's.
    ++t_depth;
    for (;;) {
        Task t;
        pthread_mutex_lock(&p.mu);
        bool got = pop(&t);
        if (!got) {
            if (__atomic_load_n(&b.done, __ATOMIC_ACQUIRE) == chunks) { pthread_mutex_unlock(&p.mu); break; }
            pthread_cond_wait(&p.cv, &p.mu);
            pthread_mutex_unlock(&p.mu);
            continue;
        }
        pthread_mutex_unlock(&p.mu);
        run_task(t);
        pthread_mutex_lock(&p.mu);
        pthread_cond_broadcast(&p.cv);
        pthread_mutex_unlock(&p.mu);
    }
    --t_depth;
}

} // namespace kv
