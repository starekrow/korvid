// Pause: a paused tick runs the Always systems and the handlers, skips the Unpaused
// systems, the timers and the triggers, and leaves the sim tick where it was; a step lets
// one sim tick through; the hash is the same whether or not a run paused along the way.
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/core/log.h"
#include "korvid/core/kind.h"

namespace {

using namespace kv;

struct Counter { i32 n; };
constinit const FieldDesc k_counter_fields[] = {KV_FIELDS(Counter, n)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Counter, k_counter_fields, CompPrev)};
constinit const KindDesc k_kinds[] = {{.name = ".thing", .components = "Counter", .capacity = 64}};

struct Ping { u32 n; };
constinit const EventTypeDesc k_events[] = {KV_EVENT(Ping)};

struct Seen { u32 sim_runs, always_runs, paused_seen, pings, edges; u64 last_tick; };
constinit Seen g = {};

// Unpaused: counts up every live Counter and schedules a Ping two ticks out, once.
void sys_sim(TickContext& ctx) {
    ++g.sim_runs;
    g.last_tick = ctx.tick;
    for (Counter& k : ctx.write<Counter>("p.thing.Counter")) k.n += 1;
    if (ctx.tick == 0) ctx.schedule_in(Ping{1}, 2);
}
void sys_always(TickContext& ctx) {
    ++g.always_runs;
    if (ctx.paused) ++g.paused_seen;
}
void on_ping(TickContext&, Span<const Ping> pings) { g.pings += pings.len; }
void on_edge(TickContext&, Span<const StateEdge> edges) { g.edges += edges.len; }

constinit const SystemDesc k_systems[] = {
    {.name = ".sim",    .phase = Phase::Sim, .fn = sys_sim, .writes = ".thing.Counter"},
    {.name = ".always", .phase = Phase::Sim, .fn = sys_always, .runs = RunWhen::Always},
};
constinit const HandlerDesc k_handlers[] = {
    {.name = ".on_ping", KV_ON(Ping, on_ping)},
    {.name = ".on_edge", KV_ON(StateEdge, on_edge)},
};
constinit const TriggerDesc k_triggers[] = {
    {.name = ".three", .field = ".thing.Counter.n", .edge = TriggerEdge::CrossedAbove, .value = 2.5},
};
KV_MODULE_DEFINE(k_p) = {.name = "p", .events = k_events, .components = k_components, .kinds = k_kinds,
                         .systems = k_systems, .handlers = k_handlers, .triggers = k_triggers};

} // namespace

void test_pause(kv::TestCtx& c) {
    using namespace kv;
    c.begin("pause");

    const ModuleDesc* const list[] = {&k_p};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    Table* things = engine_table(e, "p.thing");
    things->spawn(Counter{0});
    g = Seen{};

    // Two sim ticks, then a pause: three frames of paused ticks run Always only; the sim
    // tick holds at 2, the counter at 2, no Ping (due at tick 2) and no edge fire.
    engine_frame(e, 1.0 / 60.0);
    engine_frame(e, 1.0 / 60.0);
    KV_CHECK(c, g.sim_runs == 2 && g.always_runs == 2 && e.clock.tick_count == 2);
    engine_pause(e, true);
    for (u32 i = 0; i < 3; ++i) engine_frame(e, 1.0 / 60.0);
    KV_CHECK(c, g.sim_runs == 2 && g.always_runs == 5 && g.paused_seen == 3);
    KV_CHECK(c, e.clock.tick_count == 2 && e.clock.tick_total == 5);
    KV_CHECK(c, things->rows_of<Counter>()[0].n == 2);
    KV_CHECK(c, g.pings == 0 && g.edges == 0);

    // A step: one sim tick (tick 2) runs, the Ping due at it fires, the counter reaches 3
    // and the trigger's edge is pending; the next (paused) tick delivers the edge, since
    // handlers run on paused ticks.
    engine_step(e, 1);
    engine_frame(e, 1.0 / 60.0);
    KV_CHECK(c, g.sim_runs == 3 && g.last_tick == 2 && e.clock.tick_count == 3 && e.clock.paused);
    KV_CHECK(c, g.pings == 1 && things->rows_of<Counter>()[0].n == 3);
    engine_frame(e, 1.0 / 60.0);
    KV_CHECK(c, g.sim_runs == 3 && g.edges == 1);

    // Resume: sim ticks run again from 3.
    engine_pause(e, false);
    engine_frame(e, 1.0 / 60.0);
    KV_CHECK(c, g.sim_runs == 4 && g.last_tick == 3 && e.clock.tick_count == 4);
    u64 hash_a = engine_hash_state(e);
    engine_shutdown(e);

    // The same four sim ticks with no pause anywhere hash the same.
    Engine f;
    KV_CHECK(c, engine_init(f, list));
    engine_table(f, "p.thing")->spawn(Counter{0});
    g = Seen{};
    for (u32 i = 0; i < 4; ++i) engine_frame(f, 1.0 / 60.0);
    KV_CHECK(c, engine_hash_state(f) == hash_a && g.pings == 1 && g.edges == 1);
    KV_CHECK(c, !engine_rate(f, 0.0) && engine_rate(f, 2.0) && f.clock.rate == 2.0);
    engine_shutdown(f);
}
