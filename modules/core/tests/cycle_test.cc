// The cycles outside the tick (docs/design/cycles.md): the Frame cycle once per call, the
// Work cycle while a unit says more and the budget lasts, events across cycles delivered
// at the consumer's next run, both ways, commands at a cycle's boundary, the cycles running
// while the sim is paused, every Work unit once a cycle with the time left, streams
// trimmed without ticks.
#include "korvid/core/engine.h"
#include "korvid/core/ktable.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"

namespace {

using namespace kv;

struct Spark { i32 n; };
constinit const FieldDesc k_spark_fields[] = {KV_FIELDS(Spark, n)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Spark, k_spark_fields, 0)};
constinit const KTableDesc k_ktables[] = {{.name = ".spark", .components = "Spark", .capacity = 64, .cosmetic = true}};

struct Ticked { u64 tick; };
struct Drawn { u32 frame; };
constinit const EventTypeDesc k_events[] = {KV_EVENT(Ticked), KV_EVENT(Drawn)};

struct Seen {
    u32 ticks, frames, frame_got_ticks, sim_got_draws, work_runs, work_left, other_runs;
    f64 last_frame_dt, left_seen;
};
constinit Seen g = {};

void sys_tick(TickContext& ctx) {
    ++g.ticks;
    ctx.emit(Ticked{ctx.tick});
    g.sim_got_draws += ctx.received<Drawn>().len;
}
void sys_frame(TickContext& ctx) {
    ++g.frames;
    g.last_frame_dt = ctx.dt;
    g.frame_got_ticks += ctx.received<Ticked>().len;
    ctx.emit(Drawn{g.frames});
    ctx.spawn_later(".spark", Spark{(i32)g.frames});
}
void sys_work(TickContext& ctx) {
    ++g.work_runs;
    g.left_seen = ctx.time_left();
    if (g.work_left) { --g.work_left; if (g.work_left) ctx.more(); }
}
// A second Work unit with nothing of its own to do: it runs once a cycle all the same.
void sys_other(TickContext&) { ++g.other_runs; }

constinit const SystemDesc k_systems[] = {
    {.name = ".tick", .phase = Phase::Sim, .fn = sys_tick, .reads = "Drawn", .emits = "Ticked"},
    {.name = ".frame", .phase = Phase::Frame, .fn = sys_frame, .reads = "Ticked", .emits = "Drawn", .main_thread = true},
    {.name = ".work", .phase = Phase::Work, .fn = sys_work, .main_thread = true},
    {.name = ".other", .phase = Phase::Work, .fn = sys_other, .main_thread = true},
};
KV_MODULE_DEFINE(k_cy) = {.name = "cy", .events = k_events, .components = k_components, .ktables = k_ktables, .systems = k_systems};

} // namespace

void test_cycles(kv::TestCtx& c) {
    using namespace kv;
    c.begin("cycles");
    g = Seen{};
    const ModuleDesc* const list[] = {&k_cy};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    const f64 dt = e.clock.tick_dt;

    // Ticks run the Sim cycle only.
    engine_frame(e, dt * 3);
    KV_CHECK(c, g.ticks == 3 && g.frames == 0 && g.work_runs == 0);
    // The Frame cycle: everything the ticks emitted since its last run, and its dt.
    engine_frame_cycle(e, 0.016);
    KV_CHECK(c, g.frames == 1 && g.frame_got_ticks == 3 && g.last_frame_dt == 0.016);
    // Its spawn applied at its boundary; its event reaches the Sim at the next tick.
    KV_CHECK(c, engine_ktable(e, "cy.spark")->live() == 1);
    engine_frame(e, dt);
    KV_CHECK(c, g.sim_got_draws == 1 && g.ticks == 4);
    engine_frame_cycle(e, 0.016);
    engine_frame_cycle(e, 0.016);
    KV_CHECK(c, g.frames == 3 && g.frame_got_ticks == 4); // nothing new on the second

    // Work: once with nothing to do; again while it says more, within the budget.
    engine_work(e, 0.0);
    KV_CHECK(c, g.work_runs == 1 && g.other_runs == 1 && !engine_work_pending(e) && g.left_seen == 0.0);
    g.work_left = 5;
    g.work_runs = 0;
    engine_work(e, 1.0);
    KV_CHECK(c, g.work_runs == 5 && g.other_runs == 6 && !engine_work_pending(e) && g.left_seen > 0.4 && g.left_seen <= 0.5); // its share: half, two units to run
    // A spent budget stops it with work pending.
    g.work_left = 1000;
    g.work_runs = 0;
    engine_work(e, 0.0);
    KV_CHECK(c, g.work_runs == 1 && engine_work_pending(e) && g.work_left == 999);
    // A budget is time a unit sees left.
    g.work_left = 0;
    engine_work(e, 0.5);
    KV_CHECK(c, g.left_seen > 0.0);
    engine_frame_cycle(e, 0.016);

    // Paused, the Sim stands still and the other cycles run.
    engine_pause(e, true);
    u32 ticks = g.ticks, frames = g.frames, works = g.work_runs;
    engine_frame(e, dt * 2);
    engine_frame_cycle(e, 0.016);
    engine_work(e, 0.0);
    KV_CHECK(c, g.ticks == ticks && g.frames == frames + 1 && g.work_runs == works + 1);
    engine_pause(e, false);

    // Without ticks, the streams do not grow: what the Frame consumed is dropped.
    for (u32 i = 0; i < 200; ++i) { engine_frame(e, dt); engine_frame_cycle(e, 0.016); }
    u32 type = EventType<Ticked>::id;
    KV_CHECK(c, type < e.events.streams.len && e.events.streams[type].events.count() <= 2);
    // What a consumer has not reached is kept for it: the Sim reads Drawn, and has not run.
    for (u32 i = 0; i < 200; ++i) engine_frame_cycle(e, 0.016);
    type = EventType<Drawn>::id;
    u32 held = e.events.streams[type].events.count();
    u32 before = g.sim_got_draws;
    engine_frame(e, dt);
    KV_CHECK(c, held >= 200 && g.sim_got_draws - before == held && e.events.streams[type].events.count() <= 1);
    KV_CHECK(c, e.frame_cycles == g.frames && e.work_cycles >= 8);
    engine_shutdown(e);
}

namespace {

using namespace kv;

// A sim ktable and a cosmetic one, and units in the Frame and Work cycles that reach for them.
struct Hp { i32 n; };
struct Glow { i32 n; };
constinit const FieldDesc k_hp_fields[] = {KV_FIELDS(Hp, n)};
constinit const FieldDesc k_glow_fields[] = {KV_FIELDS(Glow, n)};
constinit const ComponentDesc k_sim_components[] = {KV_COMPONENT(Hp, k_hp_fields, 0), KV_COMPONENT(Glow, k_glow_fields, 0)};
constinit const KTableDesc k_sim_ktables[] = {
    {.name = ".unit", .components = "Hp", .capacity = 16},
    {.name = ".fx", .components = "Glow", .capacity = 16, .cosmetic = true},
};
void sys_nothing(TickContext&) {}

constinit const SystemDesc k_frame_writes_column[] = {{.name = ".bad", .phase = Phase::Frame, .fn = sys_nothing, .writes = ".unit.Hp"}};
constinit const SystemDesc k_work_writes_ktable[] = {{.name = ".bad", .phase = Phase::Work, .fn = sys_nothing, .writes = ".unit"}};
constinit const SystemDesc k_frame_writes_fx[] = {{.name = ".fine", .phase = Phase::Frame, .fn = sys_nothing, .writes = ".fx"}};
constinit const SystemDesc k_sim_writes_unit[] = {{.name = ".fine", .phase = Phase::Sim, .fn = sys_nothing, .writes = ".unit"}};
KV_MODULE_DEFINE(k_m1) = {.name = "m1", .components = k_sim_components, .ktables = k_sim_ktables, .systems = k_frame_writes_column};
KV_MODULE_DEFINE(k_m2) = {.name = "m2", .components = k_sim_components, .ktables = k_sim_ktables, .systems = k_work_writes_ktable};
KV_MODULE_DEFINE(k_m3) = {.name = "m3", .components = k_sim_components, .ktables = k_sim_ktables, .systems = k_frame_writes_fx};
KV_MODULE_DEFINE(k_m4) = {.name = "m4", .components = k_sim_components, .ktables = k_sim_ktables, .systems = k_sim_writes_unit};

struct Ping2 { u32 n; };
constinit const EventTypeDesc k_ping_events[] = {KV_EVENT(Ping2)};
void on_ping_frame(TickContext&, Span<const Ping2>) {}
constinit const HandlerDesc k_frame_handler[] = {{.name = ".on", KV_ON(Ping2, on_ping_frame), .phase = Phase::Frame, .writes = ".unit.Hp"}};
KV_MODULE_DEFINE(k_m5) = {.name = "m5", .events = k_ping_events, .components = k_sim_components, .ktables = k_sim_ktables, .handlers = k_frame_handler};

// At run time: what a Frame unit may not do by command, and may do to a cosmetic ktable.
struct Tried { Handle unit; bool done; };
constinit Tried g_try = {};
void sys_frame_commands(TickContext& ctx) {
    if (g_try.done) return;
    g_try.done = true;
    ctx.spawn_later("rt.unit", Hp{1});
    ctx.spawn_later("rt.fx", Glow{1});
    ctx.destroy(g_try.unit);
    ctx.destroy_all("rt.unit");
    ctx.schedule(Ping2{1}, ctx.tick + 3);
}
constinit const SystemDesc k_rt_systems[] = {{.name = ".cmds", .phase = Phase::Frame, .fn = sys_frame_commands, .main_thread = true}};
KV_MODULE_DEFINE(k_rt) = {.name = "rt", .events = k_ping_events, .components = k_sim_components, .ktables = k_sim_ktables, .systems = k_rt_systems};

bool starts(const ModuleDesc* m) {
    const ModuleDesc* const list[] = {m};
    Engine e;
    bool ok = engine_init(e, list);
    engine_shutdown(e);
    return ok;
}

} // namespace

void test_cycles_leave_the_sim(kv::TestCtx& c) {
    using namespace kv;
    c.begin("cycles leave the sim");
    // Refused at startup: a column of a sim ktable, a sim ktable whole, from a handler too.
    KV_CHECK(c, !starts(&k_m1) && !starts(&k_m2) && !starts(&k_m5));
    // Allowed: a cosmetic ktable from Frame; a sim ktable from the Sim.
    KV_CHECK(c, starts(&k_m3) && starts(&k_m4));

    g_try = Tried{};
    const ModuleDesc* const list[] = {&k_rt};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    g_try.unit = engine_ktable(e, "rt.unit")->spawn(Hp{5});
    u64 timers = e.timers.fired;
    engine_frame_cycle(e, 0.016);
    // The sim's ktable untouched (no spawn, no destroy), the cosmetic spawn through, no timer.
    KV_CHECK(c, engine_ktable(e, "rt.unit")->live() == 1 && engine_ktable(e, "rt.unit")->valid(g_try.unit));
    KV_CHECK(c, engine_ktable(e, "rt.fx")->live() == 1);
    engine_frame(e, e.clock.tick_dt * 5);
    KV_CHECK(c, e.timers.fired == timers);
    engine_shutdown(e);
}

namespace {

using namespace kv;

// Fair shares: a greedy unit spins until its share is gone; the unit after it still has time.
struct Fair { f64 greedy_spent, polite_left; u32 polite_runs; };
constinit Fair g_fair = {};
void sys_greedy(TickContext& ctx) {
    f64 t0 = time_now();
    while (ctx.time_left() > 0.0) {}
    g_fair.greedy_spent += time_now() - t0;
}
void sys_polite(TickContext& ctx) { g_fair.polite_left = ctx.time_left(); ++g_fair.polite_runs; }
constinit const SystemDesc k_fair_systems[] = {
    {.name = ".greedy", .phase = Phase::Work, .fn = sys_greedy, .main_thread = true},
    {.name = ".polite", .phase = Phase::Work, .fn = sys_polite, .main_thread = true},
};
KV_MODULE_DEFINE(k_fair) = {.name = "fair", .systems = k_fair_systems};

} // namespace

void test_cycles_fair(kv::TestCtx& c) {
    using namespace kv;
    c.begin("cycles fair");
    g_fair = Fair{};
    const ModuleDesc* const list[] = {&k_fair};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    engine_work(e, 0.02);
    // The greedy one had half (two units to run); the polite one found the rest waiting.
    KV_CHECK(c, g_fair.greedy_spent >= 0.009 && g_fair.greedy_spent < 0.015);
    KV_CHECK(c, g_fair.polite_runs == 1 && g_fair.polite_left > 0.005);
    engine_shutdown(e);
}

namespace {

using namespace kv;

// A stream that only a Frame unit reads, fed every tick, in a program that never runs the
// Frame cycle: held for that unit, and named once (dev builds).
struct Flood { u32 n; };
constinit const EventTypeDesc k_flood_events[] = {KV_EVENT(Flood)};
void sys_flood(TickContext& ctx) { for (u32 i = 0; i < 1000; ++i) ctx.emit(Flood{i}); }
void sys_drain(TickContext& ctx) { (void)ctx.received<Flood>(); }
constinit const SystemDesc k_flood_systems[] = {
    {.name = ".flood", .phase = Phase::Sim, .fn = sys_flood, .emits = "Flood"},
    {.name = ".drain", .phase = Phase::Frame, .fn = sys_drain, .reads = "Flood"},
};
KV_MODULE_DEFINE(k_flood) = {.name = "flood", .events = k_flood_events, .systems = k_flood_systems};

} // namespace

void test_cycles_held(kv::TestCtx& c) {
    using namespace kv;
    c.begin("cycles held");
    const ModuleDesc* const list[] = {&k_flood};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    for (u32 i = 0; i < 70; ++i) engine_frame(e, e.clock.tick_dt);
    u32 t = EventType<Flood>::id;
    KV_CHECK(c, e.events.streams[t].events.count() == 70000); // held for the drain, which has not run
#if KV_DEV
    KV_CHECK(c, t < e.events.held_warned.len && e.events.held_warned[t] == 1);
#endif
    engine_frame_cycle(e, 0.016);
    engine_frame(e, e.clock.tick_dt);
    KV_CHECK(c, e.events.streams[t].events.count() <= 1000); // taken, and dropped
    engine_shutdown(e);
}
