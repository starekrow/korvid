// IoQueue tasks: lanes hold to their limits, a worker that finishes takes the next without
// waiting for a pump, priorities order what waits, failures and cancellations complete as
// such, and free() stops what is still running.
#include "korvid/platform/io.h"
#include "korvid/core/atomic.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include <errno.h>

namespace {

using namespace kv;

struct Shared {
    u32 running = 0, most = 0, ran = 0;
    u32 order[16] = {};
    u32 next = 0;
};

struct Job {
    Shared* s;
    u32     id;
    f64     seconds; // how long it works
    i32     result;
    bool    until_cancelled;
};

i32 work(void* arg, const IoTaskCtl& ctl) {
    Job& j = *(Job*)arg;
    u32 now = atomic_add_fetch(&j.s->running, 1u, MO_SEQ_CST);
    u32 most = atomic_load(&j.s->most, MO_SEQ_CST);
    while (now > most && !atomic_compare_exchange(&j.s->most, &most, now, false, MO_SEQ_CST, MO_SEQ_CST)) {}
    u32 at = atomic_fetch_add(&j.s->next, 1u, MO_SEQ_CST);
    if (at < 16) j.s->order[at] = j.id;
    f64 end = time_now() + j.seconds;
    while (j.until_cancelled ? !ctl.cancelled() : time_now() < end) time_sleep(0.001);
    atomic_fetch_add(&j.s->ran, 1u, MO_SEQ_CST);
    atomic_sub_fetch(&j.s->running, 1u, MO_SEQ_CST);
    return j.result;
}

u32 pump_until(IoQueue& q, u32 count, f64 seconds = 5.0) {
    f64 end = time_now() + seconds;
    while (q.completions().len < count && time_now() < end) { q.pump(); time_sleep(0.001); }
    return q.completions().len;
}

const IoCompletion* by_user(const IoQueue& q, u64 user) {
    for (const IoCompletion& c : q.completions()) if (c.user == user) return &c;
    return nullptr;
}

Handle submit(IoQueue& q, Job& j, u32 lane, IoPriority prio = IoPriority::Normal) {
    IoTaskRequest r;
    r.fn = work; r.arg = &j; r.name = "job"; r.lane = (u8)lane; r.priority = prio; r.user = j.id;
    return q.run_task(r);
}

} // namespace

void test_io_tasks(TestCtx& c) {
    c.begin("io tasks");
    {
        IoQueue q;
        KV_CHECK(c, q.init(nullptr, 256, IoBackend::Sync));
        Job none{nullptr, 1, 0, 0, false};
        IoTaskRequest r; r.fn = work; r.arg = &none;
        KV_CHECK(c, q.run_task(r).is_null()); // no workers yet
        KV_CHECK(c, q.start_tasks(4) && !q.start_tasks(4));
        r.lane = kIoLanes;
        KV_CHECK(c, q.run_task(r).is_null());
        q.free();
    }
    // A lane of two, on four workers: never more than two at once, and two at once.
    // Ten jobs of 20 ms, the queue pumped only at the end: the workers refill on their own.
    {
        IoQueue q;
        KV_CHECK(c, q.init(nullptr, 256, IoBackend::Sync) && q.start_tasks(4));
        q.set_lane(1, 2);
        Shared s;
        Job jobs[10];
        for (u32 i = 0; i < 10; ++i) { jobs[i] = Job{&s, i + 1, 0.02, 0, false}; KV_CHECK(c, !submit(q, jobs[i], 1).is_null()); }
        KV_CHECK(c, q.pending() == 10);
        f64 t0 = time_now();
        while (atomic_load(&s.ran, MO_SEQ_CST) < 10 && time_now() - t0 < 5.0) time_sleep(0.002);
        f64 took = time_now() - t0;
        KV_CHECK(c, s.ran == 10 && s.most == 2);
        KV_CHECK(c, took < 0.5); // five rounds of 20 ms, not ten, and no pump between them
        KV_CHECK(c, q.completions().len == 0); // nothing completes before the pump
        q.pump();
        KV_CHECK(c, q.completions().len == 10 && q.tasks_done == 10 && q.pending() == 0);
        for (u32 i = 0; i < 10; ++i) {
            const IoCompletion* done = by_user(q, i + 1);
            KV_CHECK(c, done && done->status == IoStatus::Done && done->error == 0 && q.reap(done->request));
        }
        q.free();
    }
    // One at a time: what waits goes by priority, then by submission order. A failure is
    // Failed with its code.
    {
        IoQueue q;
        KV_CHECK(c, q.init(nullptr, 256, IoBackend::Sync) && q.start_tasks(2));
        q.set_lane(0, 1);
        Shared s;
        Job first{&s, 1, 0.05, 0, false}, low{&s, 2, 0, 0, false}, high{&s, 3, 0, EIO, false}, normal{&s, 4, 0, 0, false};
        submit(q, first, 0);
        time_sleep(0.01); // first is running
        submit(q, low, 0, IoPriority::Low);
        submit(q, normal, 0);
        submit(q, high, 0, IoPriority::High);
        KV_CHECK(c, pump_until(q, 4) == 4);
        KV_CHECK(c, s.order[0] == 1 && s.order[1] == 3 && s.order[2] == 4 && s.order[3] == 2);
        const IoCompletion* f = by_user(q, 3);
        KV_CHECK(c, f && f->status == IoStatus::Failed && f->error == EIO && q.tasks_failed == 1);
        q.free();
    }
    // Cancelled while it waits: never runs. Cancelled while it runs: it sees so and stops.
    // Both complete as Cancelled.
    {
        IoQueue q;
        KV_CHECK(c, q.init(nullptr, 256, IoBackend::Sync) && q.start_tasks(2));
        q.set_lane(0, 1);
        Shared s;
        Job busy{&s, 1, 0, 0, true}, waiting{&s, 2, 0, 0, false};
        Handle hb = submit(q, busy, 0), hw = submit(q, waiting, 0);
        time_sleep(0.02);
        KV_CHECK(c, q.cancel(hw) && q.cancel(hb));
        KV_CHECK(c, pump_until(q, 2) == 2);
        const IoCompletion* b = by_user(q, 1);
        const IoCompletion* w = by_user(q, 2);
        KV_CHECK(c, b && b->status == IoStatus::Cancelled && w && w->status == IoStatus::Cancelled);
        KV_CHECK(c, s.ran == 1); // the waiting one never ran
        KV_CHECK(c, !q.cancel(hb)); // completed
        q.free();
    }
    // free() with a task still running and more waiting: the running one is asked to stop,
    // the rest never start, and free returns.
    {
        IoQueue q;
        KV_CHECK(c, q.init(nullptr, 256, IoBackend::Sync) && q.start_tasks(1));
        Shared s;
        Job busy{&s, 1, 0, 0, true}, after{&s, 2, 0, 0, false};
        submit(q, busy, 0);
        submit(q, after, 0);
        time_sleep(0.02);
        f64 t0 = time_now();
        q.free();
        KV_CHECK(c, time_now() - t0 < 1.0 && s.ran == 1);
    }
}
