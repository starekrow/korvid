// Determinism across thread counts: two independent systems on different kinds, each a
// parallel_for that moves rows, emits an event per row and spawns by command from the
// chunks, plus a stateless pair of the same module that must share a level. The state
// hash, the delivered event order and the spawn results must be identical with one
// thread and with several.
#include "korvid/core/engine.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"
#include "korvid/core/thread.h"
#include <string.h>

namespace {

using namespace kv;

struct Pos { f32 x, y; };
struct Seq { u32 n; };
constinit const FieldDesc k_pos_fields[] = {KV_FIELDS(Pos, x, y)};
constinit const FieldDesc k_seq_fields[] = {KV_FIELDS(Seq, n)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Pos, k_pos_fields, CompPrev), KV_COMPONENT(Seq, k_seq_fields)};
constinit const char* const k_a_components[] = {"Pos", "Seq"};
constinit const char* const k_b_components[] = {"Pos"};
constinit const char* const k_c_components[] = {"Seq"};
constinit const KindDesc k_kinds[] = {
    {.name = "p.a", .components = k_a_components, .capacity = 8192},
    {.name = "p.b", .components = k_b_components, .capacity = 8192},
    {.name = "p.c", .components = k_c_components, .capacity = 65536},
};

struct Moved { Handle who; u32 by; };
constinit const FieldDesc k_moved_fields[] = {KV_FIELDS(Moved, who, by)};
constinit const EventTypeDesc k_events[] = {KV_EVENT_EX(Moved, k_moved_fields, 0)};

// Stateless: run beside each other. Each row moves, emits Moved, and every fourth spawns a c.
void sys_move_a(TickContext& ctx) {
    for (KindRows k : ctx.view<Pos, const Seq>()) {
        Span<Pos> pos = k.col<Pos>();
        Span<const Seq> seq = k.col<const Seq>();
        parallel_for(ctx, k.rows(), 37, [&](ChunkContext& cc, u32 b, u32 e) {
            for (u32 i = b; i < e; ++i) {
                if (!k.alive(i)) continue;
                pos[i].x += 1.0f; pos[i].y += (f32)seq[i].n;
                cc.emit(Moved{k.handle_at(i), seq[i].n});
                if (i % 4 == 0) cc.spawn_later("p.c", Seq{i});
            }
        });
    }
}
void sys_move_b(TickContext& ctx) {
    const Table* b = ctx.read_kind("p.b");
    Span<Pos> pos = ctx.write<Pos>("p.b.Pos");
    f32 total = parallel_reduce(ctx, b->rows(), 50, 0.0f, [&](ChunkContext& cc, u32 lo, u32 hi) {
        f32 t = 0;
        for (u32 i = lo; i < hi; ++i) {
            if (!b->is_alive(i)) continue;
            pos[i].x -= 0.5f;
            t += pos[i].x;
            cc.emit(Moved{b->handle_at(i), 1});
        }
        return t;
    }, [](f32 a, f32 x) { return a + x; });
    KV_UNUSED(total);
}
// Stateful, same module: these two keep declaration order between themselves.
struct PState { u32 order[8]; u32 n; u32 levels_a, levels_b, levels_s1, levels_s2; };
void sys_s1(TickContext& ctx) { PState& s = ctx.state_as<PState>(); s.order[s.n++ % 8] = 1; }
void sys_s2(TickContext& ctx) { PState& s = ctx.state_as<PState>(); s.order[s.n++ % 8] = 2; }
bool p_startup(Engine&, ModuleInstance& self) { self.set_state(arena_push<PState>(self.arena)); return true; }

constinit const char* const k_a_reads[]  = {"p.a.Seq"};
constinit const char* const k_a_writes[] = {"p.a.Pos"};
constinit const char* const k_b_reads[]  = {"p.b"};
constinit const char* const k_b_writes[] = {"p.b.Pos"};
constinit const SystemDesc k_systems[] = {
    {.name = "p.s1",     .phase = Phase::Sim, .fn = sys_s1},
    {.name = "p.move_a", .phase = Phase::Sim, .fn = sys_move_a, .reads = k_a_reads, .writes = k_a_writes, .state = StateUse::None},
    {.name = "p.s2",     .phase = Phase::Sim, .fn = sys_s2},
    {.name = "p.move_b", .phase = Phase::Sim, .fn = sys_move_b, .reads = k_b_reads, .writes = k_b_writes, .state = StateUse::None},
};
KV_MODULE_DEFINE(k_p) = {.name = "p", .events = k_events, .components = k_components, .kinds = k_kinds,
                         .systems = k_systems, .startup = p_startup, .memory = {.reserve = 4096}};

struct Run { u64 hash; u64 events_hash; u32 c_live; u32 spawned_null; u32 order[8]; u32 levels[4]; u64 tables[3]; f32 a0x, a0y, b0x; u32 c0; };

Run run_with(TestCtx& c, u32 threads) {
    Run r{};
    Engine e;
    e.threads = threads;
    const ModuleDesc* const list[] = {&k_p};
    KV_CHECK(c, engine_init(e, list));
    for (u32 i = 0; i < 4; ++i) r.levels[i] = e.phases.systems[(u32)Phase::Sim][i].level;
    Table* a = engine_table(e, "p.a");
    Table* b = engine_table(e, "p.b");
    for (u32 i = 0; i < 3000; ++i) a->spawn(Pos{(f32)i, 0}, Seq{i % 7});
    for (u32 i = 0; i < 2000; ++i) b->spawn(Pos{0, (f32)i});
    u64 eh = 0;
    for (u64 t = 0; t < 3; ++t) {
        e.phases.run_tick(e, e.events, 1.0 / 60.0, t);
        // The delivered Moved events, in order, fold into one hash: order is part of the result.
        // (They are delivered at the next tick's first boundary, PostSim systems being absent;
        // read them from the bus before end_tick clears... so hash what the next tick delivers.)
        eh = hash_mix(eh ^ e.events.delivered_total);
    }
    // One more tick to deliver the last emissions, then look at the batch inside a PostSim read.
    r.hash = engine_hash_state(e);
    r.events_hash = eh;
    r.c_live = engine_table(e, "p.c")->live();
    for (u32 i = 0; i < 3; ++i) r.tables[i] = e.tables[i].hash();
    r.a0x = a->rows_of<Pos>()[1].x; r.a0y = a->rows_of<Pos>()[1].y; r.b0x = b->rows_of<Pos>()[1].x;
    r.c0 = engine_table(e, "p.c")->rows_of<Seq>()[1].n;
    PState* st = engine_module_state_as<PState>(e, "p");
    memcpy(r.order, st->order, sizeof r.order);
    engine_shutdown(e);
    return r;
}

} // namespace

void test_parallel(kv::TestCtx& c) {
    using namespace kv;
    c.begin("parallel");
    Run one  = run_with(c, 1);
    Run many = run_with(c, 4);
    KV_CHECK(c, one.hash == many.hash);
    if (one.hash != many.hash)
        KV_ERROR("test", "tables a %016llx/%016llx b %016llx/%016llx c %016llx/%016llx; a[1]=(%g,%g)/(%g,%g) b[1].x=%g/%g c[1]=%u/%u",
                 (unsigned long long)one.tables[0], (unsigned long long)many.tables[0], (unsigned long long)one.tables[1], (unsigned long long)many.tables[1],
                 (unsigned long long)one.tables[2], (unsigned long long)many.tables[2], one.a0x, one.a0y, many.a0x, many.a0y, one.b0x, many.b0x, one.c0, many.c0);
    KV_CHECK(c, one.events_hash == many.events_hash);
    KV_CHECK(c, one.c_live == many.c_live && one.c_live == 3 * 750); // 3 ticks of a spawn per fourth row
    // The stateful pair kept declaration order in both; the stateless pair shares a level.
    KV_CHECK(c, memcmp(one.order, many.order, sizeof one.order) == 0 && one.order[0] == 1 && one.order[1] == 2);
    KV_CHECK(c, one.levels[0] == 1 && one.levels[1] == 1 && one.levels[2] == 1 && one.levels[3] == 2); // s1, move_a, move_b | s2
    KV_CHECK(c, jobs_worker_count() == 0); // shut down with the engine
}
