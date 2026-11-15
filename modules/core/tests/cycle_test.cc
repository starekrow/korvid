// The cycles outside the tick (docs/design/cycles.md): the Frame cycle once per call, the
// Work cycle while a unit says more and the budget lasts, events across cycles delivered
// at the consumer's next run, both ways, commands at a cycle's boundary, the cycles running
// while the sim is paused, every Work unit once a cycle with the time left, streams
// trimmed without ticks.
#include "korvid/core/engine.h"
#include "korvid/core/kind.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"

namespace {

using namespace kv;

struct Spark { i32 n; };
constinit const FieldDesc k_spark_fields[] = {KV_FIELDS(Spark, n)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Spark, k_spark_fields, 0)};
constinit const KindDesc k_kinds[] = {{.name = ".spark", .components = "Spark", .capacity = 64, .cosmetic = true}};

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
KV_MODULE_DEFINE(k_cy) = {.name = "cy", .events = k_events, .components = k_components, .kinds = k_kinds, .systems = k_systems};

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
    KV_CHECK(c, engine_table(e, "cy.spark")->live() == 1);
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
    KV_CHECK(c, g.work_runs == 5 && g.other_runs == 6 && !engine_work_pending(e) && g.left_seen > 0.5 && g.left_seen <= 1.0);
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
