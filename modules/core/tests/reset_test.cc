// The fresh sim: a run, then engine_sim_reset from a handler on a held tick and the same
// run again, is the same run as a new engine's: the same handles, the same hash at every
// tick, an event scheduled before the reset never firing; cosmetic rows stay.
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/core/kind.h"

namespace {

using namespace kv;

struct Count { i32 n; };
constinit const FieldDesc k_count_fields[] = {KV_FIELDS(Count, n)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Count, k_count_fields, 0)};
constinit const KindDesc k_kinds[] = {
    {.name = ".thing", .components = "Count", .capacity = 64},
    {.name = ".spark", .components = "Count", .capacity = 64, .cosmetic = true},
};

struct Ping { u32 n; };
struct Again { u32 n; };
constinit const EventTypeDesc k_events[] = {KV_EVENT(Ping), KV_EVENT(Again)};

struct Seen { u32 pings, resets; };
constinit Seen g = {};

// Every thing counts up; tick 0 schedules a Ping for tick 5; tick 1 spawns a second thing
// by command.
void sys_run(TickContext& ctx) {
    for (Count& k : ctx.write<Count>("r.thing.Count")) k.n += 1;
    if (ctx.tick == 0) ctx.schedule(Ping{1}, 5);
    if (ctx.tick == 1) ctx.spawn_later("r.thing", Count{100});
}
void on_ping(TickContext&, Span<const Ping> pings) { g.pings += pings.len; }
void on_reset(TickContext&, Span<const SimReset> resets) { g.resets += resets.len; }
// The game's "new run": a fresh sim, then the run's first row, as the startup made it.
void on_again(TickContext& ctx, Span<const Again>) {
    engine_sim_reset(*ctx.engine);
    ctx.write_kind("r.thing")->spawn(Count{0});
}

constinit const SystemDesc k_systems[] = {{.name = ".run", .phase = Phase::Sim, .fn = sys_run, .writes = ".thing.Count"}};
constinit const HandlerDesc k_handlers[] = {
    {.name = ".on_ping", KV_ON(Ping, on_ping), .phase = Phase::Sim},
    {.name = ".on_reset", KV_ON(SimReset, on_reset), .phase = Phase::Sim},
    {.name = ".on_again", KV_ON(Again, on_again), .phase = Phase::PreSim, .writes = ".thing"},
};
KV_MODULE_DEFINE(k_r) = {.name = "r", .events = k_events, .components = k_components, .kinds = k_kinds,
                         .systems = k_systems, .handlers = k_handlers};

constexpr f64 kDt = 1.0 / 60.0;

} // namespace

void test_reset(kv::TestCtx& c) {
    using namespace kv;
    c.begin("sim reset");
    const ModuleDesc* const list[] = {&k_r};

    // The reference: a new engine, its first row, eight ticks.
    g = Seen{};
    Engine f;
    KV_CHECK(c, engine_init(f, list));
    engine_table(f, "r.thing")->spawn(Count{0});
    u64 fresh[8];
    for (u32 i = 0; i < 8; ++i) { engine_frame(f, kDt); fresh[i] = engine_hash_state(f); }
    Table* ft = engine_table(f, "r.thing");
    Handle fresh_a = ft->handle_at(0), fresh_b = ft->handle_at(1);
    KV_CHECK(c, g.pings == 1 && ft->live() == 2 && f.clock.tick_count == 8);
    engine_shutdown(f);

    // A first run of three ticks (its Ping, due at 5, still scheduled; a second thing; a
    // spark), held, a new run from the handler, and the same eight ticks.
    g = Seen{};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    Table* things = engine_table(e, "r.thing");
    things->spawn(Count{0});
    for (u32 i = 0; i < 3; ++i) engine_frame(e, kDt);
    Handle spark = engine_table(e, "r.spark")->spawn(Count{7});
    things->destroy(things->handle_at(0)); // generations moved on: the reset must put them back
    KV_CHECK(c, things->live() == 1 && e.clock.tick_count == 3);
    engine_hold(e, HoldUi, true);
    e.events.emit(Again{1});
    engine_frame(e, kDt); // held: the handler resets and spawns; SimReset reaches its handler in the next wave
    KV_CHECK(c, e.clock.tick_count == 0 && things->live() == 1 && g.resets == 1 && e.sim_resets == 1);
    engine_hold(e, HoldUi, false);
    bool same = true;
    for (u32 i = 0; i < 8; ++i) { engine_frame(e, kDt); same = same && engine_hash_state(e) == fresh[i]; }
    KV_CHECK(c, same);
    // The same slots and generations (a second engine in a process numbers its handle
    // kinds anew, so the kind bits differ; the hash names kinds, so it does not).
    auto same_slot = [](Handle x, Handle y) { return x.index() == y.index() && x.gen() == y.gen(); };
    KV_CHECK(c, same_slot(things->handle_at(0), fresh_a) && same_slot(things->handle_at(1), fresh_b));
    KV_CHECK(c, g.pings == 1); // the new run's; the old one's went with the reset
    KV_CHECK(c, engine_table(e, "r.spark")->valid(spark));
    engine_shutdown(e);
}
