// The memory system's per-thread accounting slots: 256 of them, one per live thread. A
// thread that exits gives its slot back, so after 300 threads have come and gone a new
// one is still counted where reports look while it runs, and its totals fold on exit.
#include "korvid/core/atomic.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_thread.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"

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

} // namespace

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
