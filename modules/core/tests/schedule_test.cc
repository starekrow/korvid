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
constinit const char* const k_body_components[] = {"Pos", "Vel", "Glow"};
constinit const KindDesc k_kinds[] = {{.name = "s.body", .components = k_body_components, .capacity = 64}};

char g_trace[256];
void trace(const char* s) { strncat(g_trace, s, sizeof g_trace - strlen(g_trace) - 1); }
void sys_wrap(TickContext&)   { trace("wrap "); }
void sys_move(TickContext&)   { trace("move "); }
void sys_steer(TickContext&)  { trace("steer "); }
void sys_glow(TickContext&)   { trace("glow "); }
void sys_spawn(TickContext&)  { trace("spawn "); }
void sys_other(TickContext&)  { trace("other "); }

constinit const char* const k_pos_w[]   = {"*.Pos"};
constinit const char* const k_pos_r[]   = {"*.Pos"};
constinit const char* const k_vel_r[]   = {"*.Vel"};
constinit const char* const k_vel_w[]   = {"s.body.Vel"};
constinit const char* const k_glow_w[]  = {"*.Glow"};
constinit const char* const k_glow_r[]  = {"*.Glow"};
constinit const char* const k_body_w[]  = {"s.body"};
constinit const char* const k_after_move[] = {"s.move"};

// Declared backwards on purpose: wrap (reads Pos) before move (writes Pos, reads Vel)
// before steer (writes Vel). The schedule must come out steer, move, wrap. glow is a
// cosmetic system reading Pos: after move, and free to sit anywhere else.
constinit const SystemDesc k_a_systems[] = {
    {.name = "s.wrap",  .phase = Phase::Sim, .fn = sys_wrap,  .reads = k_pos_r, .writes = k_pos_w, .after = k_after_move},
    {.name = "s.move",  .phase = Phase::Sim, .fn = sys_move,  .reads = k_vel_r, .writes = k_pos_w},
    {.name = "s.steer", .phase = Phase::Sim, .fn = sys_steer, .writes = k_vel_w},
    {.name = "s.glow",  .phase = Phase::Sim, .fn = sys_glow,  .reads = k_pos_r, .writes = k_glow_w},
};
KV_MODULE_DEFINE(k_a) = {.name = "a", .components = k_components, .kinds = k_kinds, .systems = k_a_systems};

// A kind writer covers every column, so spawn (writes s.body) is a second writer of Vel
// and Glow and must be ordered against steer and glow by hand; move and wrap follow from
// the reads. Before move it goes by the path through steer.
constinit const char* const k_spawn_before[] = {"s.steer", "s.glow"};
constinit const SystemDesc k_b_systems[] = {
    {.name = "s.spawn", .phase = Phase::Sim, .fn = sys_spawn, .writes = k_body_w, .before = k_spawn_before},
};
KV_MODULE_DEFINE(k_b) = {.name = "b", .systems = k_b_systems};

// Rejections.
constinit const SystemDesc k_double_systems[] = {{.name = "s.other", .phase = Phase::Sim, .fn = sys_other, .writes = k_pos_w}};
KV_MODULE_DEFINE(k_double) = {.name = "double", .systems = k_double_systems};

constinit const char* const k_cyc_a_r[] = {"*.Vel"}; constinit const char* const k_cyc_a_w[] = {"*.Glow"};
constinit const char* const k_cyc_b_r[] = {"*.Glow"}; constinit const char* const k_cyc_b_w[] = {"s.body.Vel"};
constinit const SystemDesc k_cycle_systems[] = {
    {.name = "c.a", .phase = Phase::PostSim, .fn = sys_other, .reads = k_cyc_a_r, .writes = k_cyc_a_w},
    {.name = "c.b", .phase = Phase::PostSim, .fn = sys_other, .reads = k_cyc_b_r, .writes = k_cyc_b_w},
};
KV_MODULE_DEFINE(k_cycle) = {.name = "cycle", .components = k_components, .kinds = k_kinds, .systems = k_cycle_systems};

// Reads cosmetic Glow and writes sim Vel: the flow rule.
constinit const SystemDesc k_flow_systems[] = {{.name = "f.back", .phase = Phase::Sim, .fn = sys_other, .reads = k_glow_r, .writes = k_vel_w}};
KV_MODULE_DEFINE(k_flow) = {.name = "flow", .components = k_components, .kinds = k_kinds, .systems = k_flow_systems};

constinit const char* const k_after_nowhere[] = {"s.nowhere"};
constinit const SystemDesc k_unknown_systems[] = {{.name = "u.sys", .phase = Phase::Sim, .fn = sys_other, .after = k_after_nowhere}};
KV_MODULE_DEFINE(k_unknown) = {.name = "unknown", .systems = k_unknown_systems};

constinit const char* const k_after_later[] = {"s.late"};
constinit const SystemDesc k_later_systems[] = {
    {.name = "s.late",  .phase = Phase::PostSim, .fn = sys_other},
    {.name = "s.early", .phase = Phase::Sim,     .fn = sys_other, .after = k_after_later},
};
KV_MODULE_DEFINE(k_later) = {.name = "later", .systems = k_later_systems};

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

    // Derived order: writers before readers, the declared edge honored, the kind writer first.
    const ModuleDesc* const list[] = {&k_a, &k_b};
    Engine e;
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
