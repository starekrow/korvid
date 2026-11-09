// The derived schedule: readers after writers whatever the declaration order, `after`
// and `before` edges, kind writes covering their columns, the two rejections (unordered
// double writers, cycles) with names in the message, the sim-to-cosmetic flow rule, and
// the state hash: same operations, same hash; dead rows and padding invisible.
#include "korvid/core/engine.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

struct Pos  { f32 x, y; };
struct Vel  { f32 dx, dy; };
struct Glow { f32 a; };

constinit const FieldDesc k_pos_fields[]  = {KV_FIELD(Pos, x), KV_FIELD(Pos, y)};
constinit const FieldDesc k_vel_fields[]  = {KV_FIELD(Vel, dx), KV_FIELD(Vel, dy)};
constinit const FieldDesc k_glow_fields[] = {KV_FIELD(Glow, a)};
constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(Pos, k_pos_fields, CompPrev), KV_COMPONENT(Vel, k_vel_fields), KV_COMPONENT(Glow, k_glow_fields, CompCosmetic),
};
constinit const KindDesc k_kinds[] = {{.name = "s.body", .components = "Pos, Vel, Glow", .capacity = 64}};

char g_trace[256];
void trace(const char* s) { strncat(g_trace, s, sizeof g_trace - strlen(g_trace) - 1); }
void sys_wrap(TickContext&)   { trace("wrap "); }
void sys_move(TickContext&)   { trace("move "); }
void sys_steer(TickContext&)  { trace("steer "); }
void sys_glow(TickContext&)   { trace("glow "); }
void sys_spawn(TickContext&)  { trace("spawn "); }
void sys_other(TickContext&)  { trace("other "); }


// Declared backwards on purpose: wrap (reads Pos) before move (writes Pos, reads Vel)
// before steer (writes Vel). The schedule must come out steer, move, wrap. glow is a
// cosmetic system reading Pos: after move, and free to sit anywhere else.
constinit const SystemDesc k_a_systems[] = {
    {.name = "s.wrap",  .phase = Phase::Sim, .fn = sys_wrap,  .reads = "*.Pos", .writes = "*.Pos", .after = "s.move"},
    {.name = "s.move",  .phase = Phase::Sim, .fn = sys_move,  .reads = "*.Vel", .writes = "*.Pos"},
    {.name = "s.steer", .phase = Phase::Sim, .fn = sys_steer, .writes = "s.body.Vel"},
    {.name = "s.glow",  .phase = Phase::Sim, .fn = sys_glow,  .reads = "*.Pos", .writes = "*.Glow"},
};
KV_MODULE_DEFINE(k_a) = {.name = "a", .components = k_components, .kinds = k_kinds, .systems = k_a_systems};

// A kind writer covers every column, so spawn (writes s.body) is a second writer of Vel
// and Glow and must be ordered against steer and glow by hand; move and wrap follow from
// the reads. Before move it goes by the path through steer.
constinit const SystemDesc k_b_systems[] = {
    {.name = "s.spawn", .phase = Phase::Sim, .fn = sys_spawn, .writes = "s.body", .before = "s.steer, s.glow"},
};
KV_MODULE_DEFINE(k_b) = {.name = "b", .systems = k_b_systems};

// Rejections.
constinit const SystemDesc k_double_systems[] = {{.name = "s.other", .phase = Phase::Sim, .fn = sys_other, .writes = "*.Pos"}};
KV_MODULE_DEFINE(k_double) = {.name = "double", .systems = k_double_systems};

constinit const SystemDesc k_cycle_systems[] = {
    {.name = "c.a", .phase = Phase::PostSim, .fn = sys_other, .reads = "*.Vel", .writes = "*.Glow"},
    {.name = "c.b", .phase = Phase::PostSim, .fn = sys_other, .reads = "*.Glow", .writes = "s.body.Vel"},
};
KV_MODULE_DEFINE(k_cycle) = {.name = "cycle", .components = k_components, .kinds = k_kinds, .systems = k_cycle_systems};

// Reads cosmetic Glow and writes sim Vel: the flow rule.
constinit const SystemDesc k_flow_systems[] = {{.name = "f.back", .phase = Phase::Sim, .fn = sys_other, .reads = "*.Glow", .writes = "s.body.Vel"}};
KV_MODULE_DEFINE(k_flow) = {.name = "flow", .components = k_components, .kinds = k_kinds, .systems = k_flow_systems};

constinit const SystemDesc k_unknown_systems[] = {{.name = "u.sys", .phase = Phase::Sim, .fn = sys_other, .after = "s.nowhere"}};
KV_MODULE_DEFINE(k_unknown) = {.name = "unknown", .systems = k_unknown_systems};

constinit const SystemDesc k_later_systems[] = {
    {.name = "s.late",  .phase = Phase::PostSim, .fn = sys_other},
    {.name = "s.early", .phase = Phase::Sim,     .fn = sys_other, .after = "s.late"},
};
KV_MODULE_DEFINE(k_later) = {.name = "later", .systems = k_later_systems};

// An early read: think reads where the bodies are (^) and writes their Vel; move reads Vel
// and writes Pos. Without the '^' it is a cycle; with it, think runs first.
constinit const SystemDesc k_early_systems[] = {
    {.name = "e.move",  .phase = Phase::Sim, .fn = sys_move,  .reads = "*.Vel", .writes = "*.Pos"},
    {.name = "e.think", .phase = Phase::Sim, .fn = sys_steer, .reads = "^*.Pos", .writes = "*.Vel"},
};
KV_MODULE_DEFINE(k_early) = {.name = "early", .components = k_components, .kinds = k_kinds, .systems = k_early_systems};
constinit const SystemDesc k_late_read_systems[] = {
    {.name = "l.move",  .phase = Phase::Sim, .fn = sys_move,  .reads = "*.Vel", .writes = "*.Pos"},
    {.name = "l.think", .phase = Phase::Sim, .fn = sys_steer, .reads = "*.Pos", .writes = "*.Vel"},
};
KV_MODULE_DEFINE(k_late_read) = {.name = "lateread", .components = k_components, .kinds = k_kinds, .systems = k_late_read_systems};
constinit const SystemDesc k_early_write_systems[] = {{.name = "w.bad", .phase = Phase::Sim, .fn = sys_other, .writes = "^*.Pos"}};
KV_MODULE_DEFINE(k_early_write) = {.name = "earlywrite", .components = k_components, .kinds = k_kinds, .systems = k_early_write_systems};

const char* order(const Engine& e, Phase p, char* buf, usize cap) {
    buf[0] = 0;
    for (const ScheduledSystem& s : e.phases.systems[(u32)p]) { strncat(buf, s.desc->name, cap - strlen(buf) - 1); strncat(buf, " ", cap - strlen(buf) - 1); }
    return buf;
}

bool init_fails(TestCtx& c, Span<const ModuleDesc* const> list) {
    Engine f;
    bool ok = engine_init(f, list);
    if (ok) engine_shutdown(f);
    KV_CHECK(c, f.phases.system_count() == 0);
    return !ok;
}

} // namespace

void test_schedule(kv::TestCtx& c) {
    using namespace kv;
    c.begin("schedule");
    char buf[256];

    // Early reads turn the edge around.
    {
        const ModuleDesc* const early[] = {&k_early};
        Engine x;
        KV_CHECK(c, engine_init(x, early));
        KV_CHECK(c, strcmp(order(x, Phase::Sim, buf, sizeof buf), "e.think e.move ") == 0);
        engine_shutdown(x);
        const ModuleDesc* const late[] = {&k_late_read};
        KV_CHECK(c, init_fails(c, late));
        const ModuleDesc* const bad[] = {&k_early_write};
        KV_CHECK(c, init_fails(c, bad));
    }

    // Derived order: writers before readers, the declared edge honored, the kind writer first.
    const ModuleDesc* const list[] = {&k_a, &k_b};
    Engine e;
    e.threads = 1; // the trace is a global string
    KV_CHECK(c, engine_init(e, list));
    KV_CHECK(c, strcmp(order(e, Phase::Sim, buf, sizeof buf), "s.spawn s.steer s.move s.wrap s.glow ") == 0);
    g_trace[0] = 0;
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 0);
    KV_CHECK(c, strcmp(g_trace, "spawn steer move wrap glow ") == 0);

    // The hash: deterministic, sensitive to sim fields, blind to cosmetic ones and dead rows.
    Table* body = engine_table(e, "s.body");
    u64 empty = engine_hash_state(e);
    Handle h0 = body->spawn(Pos{1, 2}, Vel{3, 4}, Glow{0.5f});
    Handle h1 = body->spawn(Pos{5, 6}, Vel{7, 8}, Glow{0.5f});
    u64 two = engine_hash_state(e);
    KV_CHECK(c, two != empty);
    body->get<Glow>(h0)->a = 0.9f;
    KV_CHECK(c, engine_hash_state(e) == two); // cosmetic
    body->get<Vel>(h0)->dx = 30;
    u64 moved = engine_hash_state(e);
    KV_CHECK(c, moved != two);
    body->get<Vel>(h0)->dx = 3;
    KV_CHECK(c, engine_hash_state(e) == two);
    body->destroy(h1);
    u64 one = engine_hash_state(e);
    KV_CHECK(c, one != two && one != empty);
    Handle h2 = body->spawn(Pos{5, 6}, Vel{7, 8}, Glow{0.5f}); // same slot, new generation: a different world
    KV_CHECK(c, h2.index() == h1.index() && engine_hash_state(e) != two);
    // Hashing is on: a tick logs and records it.
    e.hash_ticks = true;
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 1);
    KV_CHECK(c, e.last_hash == engine_hash_state(e));
    engine_shutdown(e);

    // The same declarations in another module order give the same schedule.
    const ModuleDesc* const swapped[] = {&k_b, &k_a};
    e.threads = 1;
    KV_CHECK(c, engine_init(e, swapped));
    KV_CHECK(c, strcmp(order(e, Phase::Sim, buf, sizeof buf), "s.spawn s.steer s.move s.wrap s.glow ") == 0);
    engine_shutdown(e);

    // Rejections: a second writer of *.Pos with no order; a read-write cycle; the flow rule;
    // after an unknown system; after a system in a later phase.
    const ModuleDesc* const dbl[]   = {&k_a, &k_b, &k_double};
    const ModuleDesc* const cyc[]   = {&k_cycle};
    const ModuleDesc* const flow[]  = {&k_flow};
    const ModuleDesc* const unk[]   = {&k_a, &k_unknown};
    const ModuleDesc* const later[] = {&k_later};
    KV_CHECK(c, init_fails(c, dbl));
    KV_CHECK(c, init_fails(c, cyc));
    KV_CHECK(c, init_fails(c, flow));
    KV_CHECK(c, init_fails(c, unk));
    KV_CHECK(c, init_fails(c, later));
}
