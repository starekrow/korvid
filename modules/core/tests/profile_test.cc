// The profiler: zones nest and sum, waits are told apart, marks and counters land, records
// from worker threads arrive with the thread's name, a frame can be kept and dumped, the
// trace file holds every record, and the ring survives overflow with a count.
#include "korvid/core/job.h"
#include "korvid/core/memory.h"
#include "korvid/core/profile.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include <stdio.h>
#include <string.h>

#if KV_PROFILE
namespace {

using namespace kv;

void spin(u32, u32 begin, u32 end, void*) {
    KV_ZONE("spin chunk");
    volatile u64 x = 0;
    for (u32 i = begin; i < end; ++i) for (u32 k = 0; k < 2000; ++k) x += k;
    KV_MARK("chunk done");
}

const ProfStat* find(Span<const ProfStat> stats, const char* name) {
    for (const ProfStat& s : stats) if (strcmp(s.name, name) == 0) return &s;
    return nullptr;
}

} // namespace
#endif

void test_profile(kv::TestCtx& c) {
    using namespace kv;
    c.begin("profile");
#if KV_PROFILE
    const char* path = "/tmp/prof_test.json";
    prof_enable(true);
    prof_thread_name("main");
    prof_keep_frame(true);
    KV_CHECK(c, prof_trace_open(path));

    // Nested zones, a wait, a mark and a counter on this thread.
    prof_frame(1);
    {
        KV_ZONE("outer");
        { KV_ZONE("inner"); time_sleep(0.002); }
        { KV_ZONE("inner"); time_sleep(0.001); }
        { KV_ZONE_WAIT("snooze"); time_sleep(0.001); }
        KV_MARK_V("thing happened", 42);
        KV_COUNTER("things", 7);
    }
    prof_collect();
    Span<const ProfStat> st = prof_frame_stats();
    const ProfStat* outer = find(st, "outer");
    const ProfStat* inner = find(st, "inner");
    const ProfStat* snooze = find(st, "snooze");
    KV_CHECK(c, outer && inner && snooze);
    KV_CHECK(c, inner->calls == 2 && outer->calls == 1 && snooze->wait && !inner->wait);
    KV_CHECK(c, prof_ms(inner->wall) >= 3.0 && prof_ms(inner->worst) >= 2.0 && prof_ms(inner->worst) <= prof_ms(inner->wall));
    KV_CHECK(c, outer->wall >= inner->wall + snooze->wall);
    KV_CHECK(c, st.len && st[0].wall >= st[st.len - 1].wall); // sorted by wall
    bool main_seen = false;
    for (const ProfThreadStat& t : prof_thread_stats())
        if (t.name && strcmp(t.name, "main") == 0) { main_seen = true; KV_CHECK(c, t.busy >= outer->wall && t.waiting >= snooze->wall && t.open == 0); }
    KV_CHECK(c, main_seen);

    // Workers: their chunks show up under their own names, and the batch name is the zone.
    jobs_init(3);
    prof_frame(2);
    jobs_run(64, 1, spin, nullptr, "spin batch");
    prof_collect();
    st = prof_frame_stats();
    const ProfStat* batch = find(st, "spin batch");
    const ProfStat* chunk = find(st, "spin chunk");
    KV_CHECK(c, batch && chunk && batch->calls == 64 && chunk->calls == 64 && batch->wall >= chunk->wall);
    u32 workers_seen = 0;
    for (const ProfThreadStat& t : prof_thread_stats()) if (t.name && strncmp(t.name, "worker", 6) == 0 && t.busy) ++workers_seen;
    KV_CHECK(c, workers_seen >= 1);
    jobs_shutdown();
    prof_collect(); // the dead rings are drained and their slots freed

    // Overflow: far more records than a ring holds, then a collect that reports the loss.
    prof_frame(3);
    for (u32 i = 0; i < 80000; ++i) KV_MARK("flood");
    prof_collect();
    u64 dropped = 0;
    for (const ProfThreadStat& t : prof_thread_stats()) if (t.name && strcmp(t.name, "main") == 0) dropped = t.dropped;
    KV_CHECK(c, dropped > 0 && dropped < 80000);

    // The kept frame dumps without complaint; the summary too.
    prof_log_frame(0);
    prof_log_summary(5);
    prof_trace_close();
    FILE* f = fopen(path, "r");
    KV_CHECK(c, f != nullptr);
    if (f) {
        u32 events = 0, names = 0;
        char line[512];
        while (fgets(line, sizeof line, f)) { if (strstr(line, "\"ph\":")) ++events; if (strstr(line, "thread_name")) ++names; }
        fclose(f);
        KV_CHECK(c, events > 100 && names >= 3); // main, workers, the frames track
        remove(path);
    }
    prof_shutdown();
    KV_CHECK(c, !prof_enabled());
#else
    KV_CHECK(c, true);
#endif
}
