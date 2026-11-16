// The job pool: every chunk runs exactly once with the right range, on workers and the
// caller; a batch started inside a chunk completes (the waiter helps); the pool restarts
// with another count; zero workers runs everything on the caller.
#include "korvid/core/job.h"
#include "korvid/core/atomic.h"
#include "korvid/core/test.h"
#include "korvid/core/thread.h"
#include <string.h>

namespace {

using namespace kv;

struct Sum { u32* hits; u32 count; u32 thread_of[16]; }; // which thread ran each chunk, last batch

void mark(u32 chunk, u32 begin, u32 end, void* user) {
    Sum& s = *(Sum*)user;
    for (u32 i = begin; i < end; ++i) kv::atomic_add_fetch(&s.hits[i], 1, kv::MO_RELAXED);
    // Enough work per chunk that a worker can wake and take one before the caller has done them all.
    volatile u64 spin = 0;
    for (u32 k = 0; k < 20000; ++k) spin += k;
    if (chunk < 16) kv::atomic_store(&s.thread_of[chunk], thread_id(), kv::MO_RELAXED);
}

// Distinct threads among the last batch's chunks. Thread ids are process-wide and climb
// with every engine the suite has started, so they are compared, never indexed.
u32 distinct_threads(const Sum& s) {
    u32 ids[16], n = 0;
    for (u32 c = 0; c < 16; ++c) {
        u32 t = s.thread_of[c];
        if (!t) continue;
        bool known = false;
        for (u32 k = 0; k < n; ++k) known = known || ids[k] == t;
        if (!known) ids[n++] = t;
    }
    return n;
}

struct Nested { u32* inner_hits; };
void outer(u32, u32 begin, u32 end, void* user) {
    Nested& n = *(Nested*)user;
    for (u32 i = begin; i < end; ++i) {
        Sum s{n.inner_hits + i * 100, 100, {}};
        jobs_run(100, 7, mark, &s); // a batch from inside a chunk
    }
}

} // namespace

void test_job(kv::TestCtx& c) {
    using namespace kv;
    c.begin("job");

    u32 hits[1000];
    const u32 counts[] = {0, 3};
    for (u32 workers : counts) {
        KV_CHECK(c, jobs_init(workers) && jobs_worker_count() == workers);
        memset(hits, 0, sizeof hits);
        Sum s{hits, 1000, {}};
        jobs_run(1000, 64, mark, &s);
        bool all_once = true;
        for (u32 h : hits) all_once = all_once && h == 1;
        KV_CHECK(c, all_once);
        // Over many small batches some chunk lands on a worker; one batch could finish on
        // the caller before a worker wakes, and on a machine busy with other tests (all of
        // kormake test at once) twenty did not always.
        u32 threads = 1;
        for (u32 rep = 0; rep < 200 && threads < 2; ++rep) { jobs_run(1000, 64, mark, &s); threads = distinct_threads(s); }
        KV_CHECK(c, workers == 0 ? threads == 1 : threads >= 2); // the caller plus at least one worker
        // Nested: 8 outer chunks each running a 100-element batch.
        u32 inner[800];
        memset(inner, 0, sizeof inner);
        Nested n{inner};
        jobs_run(8, 1, outer, &n);
        all_once = true;
        for (u32 h : inner) all_once = all_once && h == 1;
        KV_CHECK(c, all_once);
        // Empty and single-chunk batches.
        memset(hits, 0, sizeof hits);
        jobs_run(0, 10, mark, &s);
        jobs_run(5, 10, mark, &s);
        KV_CHECK(c, hits[0] == 1 && hits[4] == 1 && hits[5] == 0);
    }
    jobs_shutdown();
    KV_CHECK(c, jobs_worker_count() == 0);
}
