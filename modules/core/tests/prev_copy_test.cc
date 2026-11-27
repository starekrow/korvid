// The previous copy of a large ktable goes to the job pool (KTable::begin_tick): every row
// of every prev column is copied exactly once whatever the thread count, a ragged last
// chunk included; a column without prev is left alone; a small ktable is copied on the
// caller; and the tick runner's copy is the same one.
#include "korvid/core/engine.h"
#include "korvid/core/job.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"

namespace {

using namespace kv;

struct Pos { f32 x, y; };
struct Wide { u64 a, b, c; };
struct Plain { u32 n; };
constinit const FieldDesc k_pos_fields[] = {KV_FIELDS(Pos, x, y)};
constinit const FieldDesc k_wide_fields[] = {KV_FIELDS(Wide, a, b, c)};
constinit const FieldDesc k_plain_fields[] = {KV_FIELDS(Plain, n)};
constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(Pos, k_pos_fields, CompPrev), KV_COMPONENT(Wide, k_wide_fields, CompPrev), KV_COMPONENT(Plain, k_plain_fields),
};
// 70,001 rows of 32 bytes of prev: over the pool's threshold, and not a whole number of chunks.
constexpr u32 kBig = 70001, kSmall = 10;
constinit const KTableDesc k_ktables[] = {
    {.name = "pc.big", .components = "Pos, Wide, Plain", .capacity = kBig},
    {.name = "pc.small", .components = "Pos", .capacity = 64},
};
KV_MODULE_DEFINE(k_pc) = {.name = "pc", .components = k_components, .ktables = k_ktables};

// Rows whose previous copies are not what `round` wrote into the current ones.
u32 wrong_rows(const KTable& t, u32 round) {
    Span<const Pos> pos = t.prev_rows_of<Pos>();
    Span<const Wide> wide = t.prev_rows_of<Wide>();
    u32 wrong = 0;
    for (u32 i = 0; i < pos.len; ++i) {
        bool ok = pos[i].x == (f32)i + 0.5f * (f32)round && pos[i].y == -(f32)i;
        ok = ok && wide[i].a == i && wide[i].b == (u64)i * 7 + round && wide[i].c == ~(u64)i;
        wrong += !ok;
    }
    return wrong;
}
void write_rows(KTable& t, u32 round) {
    Span<Pos> pos = t.rows_of<Pos>();
    Span<Wide> wide = t.rows_of<Wide>();
    for (u32 i = 0; i < pos.len; ++i) {
        pos[i] = Pos{(f32)i + 0.5f * (f32)round, -(f32)i};
        wide[i] = Wide{i, (u64)i * 7 + round, ~(u64)i};
    }
}

} // namespace

void test_prev_copy(kv::TestCtx& c) {
    using namespace kv;
    c.begin("prev_copy");

    const u32 thread_counts[] = {4, 1};
    for (u32 threads : thread_counts) {
        const ModuleDesc* const list[] = {&k_pc};
        Engine e;
        e.threads = threads;
        KV_CHECK(c, engine_init(e, list));
        KTable* big = engine_ktable(e, "pc.big");
        KTable* small = engine_ktable(e, "pc.small");
        KV_CHECK(c, big && small && jobs_worker_count() == threads - 1);
        for (u32 i = 0; i < kBig; ++i) big->spawn();
        for (u32 i = 0; i < kSmall; ++i) small->spawn();
        KV_CHECK(c, big->rows() == kBig && big->prev_rows_of<Pos>().len == kBig && big->column<Plain>()->prev == nullptr);

        // The large one: a batch on the pool when there are workers, and every row right.
        write_rows(*big, 1);
        jobs_stats_reset();
        big->begin_tick();
        JobStats st = jobs_stats();
        KV_CHECK(c, st.batches == (threads > 1 ? 1u : 0u));
        write_rows(*big, 2); // the current rows move on; the copies must not
        KV_CHECK(c, wrong_rows(*big, 1) == 0);

        // The small one: copied on the caller, no batch.
        small->rows_of<Pos>()[kSmall - 1] = Pos{7, 8};
        jobs_stats_reset();
        small->begin_tick();
        KV_CHECK(c, jobs_stats().batches == 0 && small->prev_rows_of<Pos>()[kSmall - 1].x == 7);

        // The tick runner's copy, at the top of PreSim, is this one.
        e.phases.run_tick(e, e.events, 1.0 / 60.0, 0);
        write_rows(*big, 3);
        KV_CHECK(c, wrong_rows(*big, 2) == 0);
        engine_shutdown(e);
    }
}
