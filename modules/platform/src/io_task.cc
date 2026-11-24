// Korvid platform: IoQueue's tasks. Workers of the queue's own (core's OS threads) take
// tasks from per-lane, per-priority lists as they come free, run them, and post them to a
// finished list that the pump moves onto the queue's completion list. A task's node is
// separate from its IoEntry: the entry lives in the queue's pool, which the queue's thread
// may grow while a worker runs, and the workers touch only nodes and the lists, under one
// lock. The queue's thread submits, cancels and completes; the workers do the rest.
#include "io_task.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_thread.h"
#include <errno.h>
#include <string.h>

namespace kv {

namespace {
constexpr u32 kPriorities = 4;            // IoPriority's
constexpr u32 kStatLane   = kIoLanes;     // stat_file's, past the callers' lanes: never limited
constexpr u32 kLanes      = kIoLanes + 1;

// A stat on a worker: the path, copied (the entry may move while the worker runs), and the
// answer.
struct StatJob { IoStat st; char path[1]; };

i32 stat_job(void* arg, const IoTaskCtl&) {
    StatJob& j = *(StatJob*)arg;
    return io_stat_now(j.path, &j.st);
}
} // namespace

struct IoTask {
    IoTaskFn  fn;
    void*     arg;
    Handle    h;
    u32       lane;
    u32       prio;
    u64       seq;
    IoTaskCtl ctl;
    i32       result;
    bool      ran;
    bool      stat;    // a stat_file's: `arg` is its StatJob, the queue's to free
    IoTask*   next;
};

struct IoWorker {
    OsThread thread;
    IoTasks* owner;
    IoTask*  current; // what it runs: asked to stop at free()
};

struct IoTasks {
    OsMutex   lock;
    OsCond    work;                           // a task, a free lane slot, or stop
    IoTask*   head[kLanes][kPriorities];      // waiting, oldest first
    IoTask*   tail[kLanes][kPriorities];
    u32       running[kLanes];
    u32       limit[kLanes];                  // 0: no limit but the workers
    IoTask*   done_head;                      // finished, for the pump, oldest first
    IoTask*   done_tail;
    bool      stop;
    IoWorker* workers;
    u32       count;
};

namespace {

// The best task a worker may take now: the highest priority, then the oldest, of the
// lanes with room. Unlinked from its list. Under the lock.
IoTask* take(IoTasks& t) {
    IoTask* best = nullptr;
    u32 bl = 0, bp = 0;
    for (u32 l = 0; l < kLanes; ++l) {
        if (t.limit[l] && t.running[l] >= t.limit[l]) continue;
        for (u32 p = kPriorities; p-- > 0;) {
            IoTask* c = t.head[l][p];
            if (!c) continue;
            if (!best || p > bp || (p == bp && c->seq < best->seq)) { best = c; bl = l; bp = p; }
            break; // a lane's best is its highest non-empty priority
        }
    }
    if (!best) return nullptr;
    t.head[bl][bp] = best->next;
    if (!best->next) t.tail[bl][bp] = nullptr;
    best->next = nullptr;
    return best;
}

void finish(IoTasks& t, IoTask* task) {
    task->next = nullptr;
    if (t.done_tail) t.done_tail->next = task; else t.done_head = task;
    t.done_tail = task;
}

void worker_main(void* arg) {
    IoWorker& w = *(IoWorker*)arg;
    IoTasks& t = *w.owner;
    os_mutex_lock(&t.lock);
    for (;;) {
        IoTask* task = nullptr;
        while (!t.stop && !(task = take(t))) os_cond_wait(&t.work, &t.lock);
        if (!task) break; // stopping: what still waits is freed by io_tasks_stop
        ++t.running[task->lane];
        w.current = task;
        os_mutex_unlock(&t.lock);
        if (!task->ctl.cancelled()) {
            task->ran = true;
            task->result = task->fn(task->arg, task->ctl);
        }
        os_mutex_lock(&t.lock);
        w.current = nullptr;
        --t.running[task->lane];
        finish(t, task);
        // A slot in that lane came free: a worker waiting on a full lane may go now.
        os_cond_signal(&t.work);
    }
    os_mutex_unlock(&t.lock);
}

} // namespace

namespace {

// A node onto its lane's list, for the next worker free.
void enqueue(IoQueue& q, IoTask* task) {
    ++q.tasks_pending;
    os_mutex_lock(&q.tasks->lock);
    IoTask*& tl = q.tasks->tail[task->lane][task->prio];
    if (tl) tl->next = task; else q.tasks->head[task->lane][task->prio] = task;
    tl = task;
    os_cond_signal(&q.tasks->work);
    os_mutex_unlock(&q.tasks->lock);
}

} // namespace

bool IoQueue::start_tasks(u32 workers) {
    if (tasks || !workers) return false;
    IoTasks* t = (IoTasks*)mem_alloc(sizeof(IoTasks));
    if (!t) return false;
    *t = IoTasks{};
    os_mutex_init(&t->lock);
    os_cond_init(&t->work);
    t->workers = (IoWorker*)mem_alloc(sizeof(IoWorker) * workers);
    for (u32 i = 0; i < workers; ++i) t->workers[i] = IoWorker{};
    tasks = t;
    for (u32 i = 0; i < workers; ++i) {
        t->workers[i].owner = t;
        if (!os_thread_start(&t->workers[i].thread, worker_main, &t->workers[i])) {
            KV_ERROR("io", "task worker %u of %u did not start", i + 1, workers);
            break;
        }
        ++t->count;
    }
    if (!t->count) { io_tasks_stop(*this); return false; }
    return true;
}

void IoQueue::set_lane(u32 lane, u32 limit) {
    if (!tasks || lane >= kIoLanes) return;
    os_mutex_lock(&tasks->lock);
    tasks->limit[lane] = limit;
    os_cond_broadcast(&tasks->work); // raised: room for more now
    os_mutex_unlock(&tasks->lock);
}

Handle IoQueue::run_task(const IoTaskRequest& req) {
    if (!tasks) { KV_ERROR("io", "task '%s': the queue has no task workers (start_tasks)", req.name ? req.name : ""); return Handle{}; }
    if (!req.fn || req.lane >= kIoLanes) { KV_ERROR("io", "task '%s': no function, or no lane %u", req.name ? req.name : "", req.lane); return Handle{}; }
    IoEntry e{};
    if (req.name) strncpy(e.path, req.name, sizeof e.path - 1);
    e.priority = req.priority;
    e.status   = IoStatus::Pending;
    e.user     = req.user;
    e.sequence = next_sequence++;
    e.fd = KHandle{};
    e.in_flight = true; // the workers have it; nothing of the ring's applies
    IoTask* task = (IoTask*)mem_alloc(sizeof(IoTask));
    if (!task) return Handle{};
    *task = IoTask{};
    e.task = task;
    Handle h = entries.create(e);
    if (h.is_null()) { mem_free(task); KV_ERROR("io", "queue full"); return h; }
    task->fn = req.fn;
    task->arg = req.arg;
    task->h = h;
    task->lane = req.lane;
    task->prio = (u32)req.priority;
    task->seq = e.sequence;
    enqueue(*this, task);
    return h;
}

bool io_tasks_stat(IoQueue& q, Handle h, IoEntry& e) {
    const char* path = e.long_path ? e.long_path : e.path;
    usize n = strlen(path);
    StatJob* job = (StatJob*)mem_alloc(sizeof(StatJob) + n);
    IoTask* task = job ? (IoTask*)mem_alloc(sizeof(IoTask)) : nullptr;
    if (!task) { mem_free(job); return false; }
    *job = StatJob{};
    memcpy(job->path, path, n + 1);
    *task = IoTask{};
    task->fn = stat_job;
    task->arg = job;
    task->h = h;
    task->lane = kStatLane;
    task->prio = (u32)e.priority;
    task->seq = e.sequence;
    task->stat = true;
    e.task = task;
    e.in_flight = true;
    enqueue(q, task);
    return true;
}

void io_tasks_complete(IoQueue& q) {
    IoTasks* t = q.tasks;
    if (!t) return;
    os_mutex_lock(&t->lock);
    IoTask* list = t->done_head;
    t->done_head = t->done_tail = nullptr;
    os_mutex_unlock(&t->lock);
    while (list) {
        IoTask* task = list;
        list = task->next;
        IoCompletion c{};
        c.request = task->h;
        IoEntry* e = q.entries.get(task->h);
        bool cancelled = task->ctl.cancelled();
        c.status = cancelled ? IoStatus::Cancelled : task->result ? IoStatus::Failed : IoStatus::Done;
        c.error = cancelled ? (task->result ? task->result : ECANCELED) : task->result;
        if (e) {
            c.user = e->user;
            e->status = c.status;
            e->task = nullptr;
            e->in_flight = false;
        }
        if (task->stat) { // a stat_file's: its answer, and the stat counts
            if (c.status == IoStatus::Done) { c.stat = ((StatJob*)task->arg)->st; ++q.stats_done; }
            else if (c.status == IoStatus::Failed) ++q.stats_failed;
            if (c.status == IoStatus::Cancelled) c.error = 0;
            mem_free(task->arg);
        }
        else if (c.status == IoStatus::Done) ++q.tasks_done;
        else if (c.status == IoStatus::Failed) ++q.tasks_failed;
        --q.tasks_pending;
        q.completed.push(c);
        mem_free(task);
    }
}

bool io_tasks_cancel(IoQueue& q, IoEntry& e) {
    if (!q.tasks || !e.task) return false;
    atomic_store(&e.task->ctl.flag, 1u, MO_RELAXED);
    return true;
}

void io_tasks_stop(IoQueue& q) {
    IoTasks* t = q.tasks;
    if (!t) return;
    os_mutex_lock(&t->lock);
    t->stop = true;
    for (u32 i = 0; i < t->count; ++i) if (t->workers[i].current) atomic_store(&t->workers[i].current->ctl.flag, 1u, MO_RELAXED);
    os_cond_broadcast(&t->work);
    os_mutex_unlock(&t->lock);
    for (u32 i = 0; i < t->count; ++i) os_thread_join(&t->workers[i].thread);
    // Never started, or finished and never pumped: their completions will not be wanted.
    auto drop = [](IoTask* task) { if (task->stat) mem_free(task->arg); mem_free(task); };
    for (u32 l = 0; l < kLanes; ++l)
        for (u32 p = 0; p < kPriorities; ++p)
            for (IoTask* task = t->head[l][p]; task;) { IoTask* n = task->next; drop(task); task = n; }
    for (IoTask* task = t->done_head; task;) { IoTask* n = task->next; drop(task); task = n; }
    os_cond_destroy(&t->work);
    os_mutex_destroy(&t->lock);
    mem_free(t->workers);
    mem_free(t);
    q.tasks = nullptr;
    q.tasks_pending = 0;
}

} // namespace kv
