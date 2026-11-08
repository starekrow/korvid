// Scheduled events and state triggers: a timer fires at the first boundary of its tick
// and is visible to the Input phase; the past means the next tick; far ticks cross the
// ring's horizon; a Changed trigger fires once per change, a CrossedBelow once per
// crossing, neither on spawn; bad trigger declarations are rejected.
#include "korvid/core/engine.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

struct Hp  { i32 hp; };
struct Pos { f32 x, y; };
constinit const FieldDesc k_hp_fields[]  = {KV_FIELD(Hp, hp)};
constinit const FieldDesc k_pos_fields[] = {KV_FIELD(Pos, x), KV_FIELD(Pos, y)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Hp, k_hp_fields, CompPrev), KV_COMPONENT(Pos, k_pos_fields)};
constinit const KindDesc k_kinds[] = {{.name = "t.ship", .components = "Hp, Pos", .capacity = 8}};

struct Alarm { u32 n; };
constinit const FieldDesc k_alarm_fields[] = {KV_FIELD(Alarm, n)};
constinit const EventTypeDesc k_events[] = {KV_EVENT_EX(Alarm, k_alarm_fields, 0)};

constinit u32 g_dead_id = 0;
constinit const TriggerDesc k_triggers[] = {
    {.name = "t.dead",    .field = "t.ship.Hp.hp", .edge = TriggerEdge::CrossedBelow, .value = 1.0, .id = &g_dead_id},
    {.name = "t.changed", .field = "t.ship.Hp.hp", .edge = TriggerEdge::Changed},
};

struct Trace { u32 alarms_in_input; u32 alarm_n; u64 alarm_tick; u32 dead_edges; u32 changed_edges; Handle dead_entity; f64 dead_value; };
constinit Trace g = {};

void sys_input(TickContext& ctx) {
    for (const Alarm& a : ctx.events->read<Alarm>()) { ++g.alarms_in_input; g.alarm_n = a.n; g.alarm_tick = ctx.tick; }
    for (const StateEdge& s : ctx.events->read<StateEdge>()) {
        if (s.trigger == g_dead_id) { ++g.dead_edges; g.dead_entity = s.entity; g.dead_value = s.value; }
        else ++g.changed_edges;
    }
}
constinit const SystemDesc k_systems[] = {{.name = "t.input", .phase = Phase::Input, .fn = sys_input}};
KV_MODULE_DEFINE(k_t) = {.name = "t", .events = k_events, .components = k_components, .kinds = k_kinds,
                         .systems = k_systems, .triggers = k_triggers};

constinit const TriggerDesc k_noprev_triggers[] = {{.name = "b.noprev", .field = "t.ship.Pos.x"}};
KV_MODULE_DEFINE(k_noprev) = {.name = "noprev", .triggers = k_noprev_triggers};
constinit const TriggerDesc k_nofield_triggers[] = {{.name = "b.nofield", .field = "t.ship.Hp.mana"}};
KV_MODULE_DEFINE(k_nofield) = {.name = "nofield", .triggers = k_nofield_triggers};
constinit const TriggerDesc k_nokind_triggers[] = {{.name = "b.nokind", .field = "t.boat.Hp.hp"}};
KV_MODULE_DEFINE(k_nokind) = {.name = "nokind", .triggers = k_nokind_triggers};

} // namespace

void test_trigger(kv::TestCtx& c) {
    using namespace kv;
    c.begin("trigger");

    const ModuleDesc* const list[] = {&k_t};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KV_CHECK(c, e.triggers.len == 2 && g_dead_id == 1 && engine_trigger_id(e, "t.changed") == 2);

    // Timers, scheduled from outside the schedule through a bare context.
    TickContext ctx{};
    ctx.engine = &e;
    ctx.events = &e.events;
    g = Trace{};
    u32 t1 = ctx.schedule(Alarm{1}, 3);
    u32 t2 = ctx.schedule(Alarm{2}, 0);          // the past: fires on the next tick run
    u32 t3 = ctx.schedule(Alarm{3}, 5000);       // beyond the ring: filed when the ring wraps
    KV_CHECK(c, t1 && t2 && t3 && t1 != t2 && e.timers.pending == 3);
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 0);
    KV_CHECK(c, g.alarms_in_input == 1 && g.alarm_n == 2 && g.alarm_tick == 0); // the past one, at tick 0's first boundary
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 1);
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 2);
    KV_CHECK(c, g.alarms_in_input == 1);
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 3);
    KV_CHECK(c, g.alarms_in_input == 2 && g.alarm_n == 1 && g.alarm_tick == 3 && e.timers.pending == 1);
    for (u64 t = 4; t < 5000; ++t) e.phases.run_tick(e, e.events, 1.0 / 60.0, t);
    KV_CHECK(c, g.alarms_in_input == 2);
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 5000);
    KV_CHECK(c, g.alarms_in_input == 3 && g.alarm_n == 3 && e.timers.pending == 0 && e.timers.fired == 3);
    // Cancel: a near one, a far one, one that already fired, and one that never was.
    u32 near = ctx.schedule(Alarm{4}, 5003);
    u32 keep = ctx.schedule(Alarm{5}, 5003);
    u32 far  = ctx.schedule(Alarm{6}, 9000);
    KV_CHECK(c, e.timers.pending == 3 && ctx.cancel(near) && ctx.cancel(far) && e.timers.pending == 1);
    KV_CHECK(c, !ctx.cancel(near) && !ctx.cancel(t1) && !ctx.cancel(0));
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 5001);
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 5002);
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 5003);
    KV_CHECK(c, g.alarms_in_input == 4 && g.alarm_n == 5 && e.timers.pending == 0 && e.timers.cancelled == 2);
    KV_CHECK(c, !ctx.cancel(keep));

    // Triggers. Spawning is not an edge. Then hp 3 -> 2 is a change; 2 -> 0 is a change and a death.
    Table* ships = engine_table(e, "t.ship");
    g = Trace{};
    Handle s0 = ships->spawn(Hp{3}, Pos{});
    u64 tick = 5004;
    e.phases.run_tick(e, e.events, 1.0 / 60.0, tick++);
    e.phases.run_tick(e, e.events, 1.0 / 60.0, tick++); // delivers edges from the previous tick, if any
    KV_CHECK(c, g.dead_edges == 0 && g.changed_edges == 0 && e.edges_fired == 0);
    // A change inside the tick: write through the Input system's slot is awkward from here,
    // so poke after PreSim's copy by running two ticks around the write.
    ships->get<Hp>(s0)->hp = 2; // between ticks: PreSim will copy it into prev before anything compares. Not an edge.
    e.phases.run_tick(e, e.events, 1.0 / 60.0, tick++);
    e.phases.run_tick(e, e.events, 1.0 / 60.0, tick++);
    KV_CHECK(c, g.changed_edges == 0);
    // Make the write land after the copy: a system would do this; emulate by writing prev by hand.
    *ships->prev<Hp>(s0) = Hp{3};
    engine_fire_triggers(e, tick);                        // hp 3 -> 2: changed, not dead
    e.phases.run_tick(e, e.events, 1.0 / 60.0, tick++);  // delivers the StateEdge
    KV_CHECK(c, g.changed_edges == 1 && g.dead_edges == 0 && e.edges_fired == 1);
    ships->get<Hp>(s0)->hp = 0;
    *ships->prev<Hp>(s0) = Hp{2};
    engine_fire_triggers(e, tick);                        // 2 -> 0: changed and crossed below 1
    e.phases.run_tick(e, e.events, 1.0 / 60.0, tick++);
    KV_CHECK(c, g.changed_edges == 2 && g.dead_edges == 1 && g.dead_entity == s0 && g.dead_value == 0.0);
    // Staying dead is no new edge; coming back up is a change but not a death.
    *ships->prev<Hp>(s0) = Hp{0};
    engine_fire_triggers(e, tick);
    e.phases.run_tick(e, e.events, 1.0 / 60.0, tick++);
    KV_CHECK(c, g.changed_edges == 2 && g.dead_edges == 1);
    ships->get<Hp>(s0)->hp = 5;
    engine_fire_triggers(e, tick);
    e.phases.run_tick(e, e.events, 1.0 / 60.0, tick++);
    KV_CHECK(c, g.changed_edges == 3 && g.dead_edges == 1);
    // A StateEdge about an entity destroyed before delivery is dropped.
    ships->get<Hp>(s0)->hp = 0;
    *ships->prev<Hp>(s0) = Hp{5};
    engine_fire_triggers(e, tick);
    ships->destroy(s0);
    e.phases.run_tick(e, e.events, 1.0 / 60.0, tick++);
    KV_CHECK(c, g.dead_edges == 1 && g.changed_edges == 3 && e.events.dropped_stale == 2);

    engine_shutdown(e);
    KV_CHECK(c, g_dead_id == 0 && e.triggers.len == 0);

    // Rejections: no previous copy, unknown field, unknown kind.
    const ModuleDesc* const noprev[]  = {&k_t, &k_noprev};
    const ModuleDesc* const nofield[] = {&k_t, &k_nofield};
    const ModuleDesc* const nokind[]  = {&k_t, &k_nokind};
    const Span<const ModuleDesc* const> rejected[] = {noprev, nofield, nokind};
    for (Span<const ModuleDesc* const> l : rejected) {
        Engine f;
        KV_CHECK(c, !engine_init(f, l));
        KV_CHECK(c, f.triggers.len == 0);
    }
}
