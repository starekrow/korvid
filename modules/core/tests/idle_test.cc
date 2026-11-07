#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"

namespace {

struct Counter { kv::u32 calls = 0; kv::u32 left = 0; kv::IdleResult when_empty = kv::IdleResult::Nothing; };

kv::IdleResult count_task(kv::Engine&, kv::f64, void* user) {
    Counter& c = *(Counter*)user;
    ++c.calls;
    if (c.left == 0) return c.when_empty;
    --c.left;
    return kv::IdleResult::More;
}

kv::IdleResult slow_task(kv::Engine&, kv::f64 budget, void* user) {
    Counter& c = *(Counter*)user;
    ++c.calls;
    kv::time_sleep(budget > 0.002 ? 0.002 : budget); // a slice that uses what it was given, up to 2 ms
    return kv::IdleResult::More;
}

} // namespace

void test_idle(kv::TestCtx& c) {
    using namespace kv;
    c.begin("idle");
    Engine e;
    KV_CHECK(c, engine_init(e, Span<const ModuleDesc* const>()));

    // No tasks: nothing happens and no time is spent.
    KV_CHECK(c, engine_idle_run(e, 0.01) == 0.0 && e.idle_stats.runs == 0);

    // Two tasks share rounds; one finishes early and is removed; the other says Nothing
    // once it is out of work, which ends the run before the budget is used.
    Counter a, b;
    a.left = 2; a.when_empty = IdleResult::Done;
    b.left = 5; b.when_empty = IdleResult::Nothing;
    u32 ia = engine_idle_add(e, "a", count_task, &a);
    u32 ib = engine_idle_add(e, "b", count_task, &b);
    KV_CHECK(c, ia != 0 && ib != 0 && ia != ib && e.idle.len == 2);
    f64 used = engine_idle_run(e, 1.0);
    KV_CHECK(c, used < 0.1);                      // far under budget: everyone ran out of work
    KV_CHECK(c, a.calls == 3 && b.calls == 6);    // a: 2 slices then Done; b: 5 slices then Nothing
    KV_CHECK(c, e.idle.len == 1 && e.idle[0].id == ib && !engine_idle_remove(e, ia));
    KV_CHECK(c, e.idle_stats.runs == 1 && e.idle_stats.calls == 9);

    // A task that always has more work is cut off by the budget.
    Counter s;
    u32 is = engine_idle_add(e, "slow", slow_task, &s);
    f64 t0 = time_now();
    used = engine_idle_run(e, 0.010);
    f64 wall = time_now() - t0;
    KV_CHECK(c, s.calls >= 3 && s.calls <= 8);    // ~2 ms slices in a 10 ms budget
    KV_CHECK(c, used >= 0.008 && wall < 0.05);    // used what it was given, stopped soon after
    KV_CHECK(c, b.calls >= 6 + s.calls - 1 && b.calls <= 6 + s.calls + 1); // the other task gets its turn each round
    KV_CHECK(c, engine_idle_remove(e, is) && e.idle.len == 1);

    engine_shutdown(e);
    KV_CHECK(c, e.idle.len == 0);
}
