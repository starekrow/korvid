// Data access through the TickContext: resolved sets with wildcards expanded, columns by
// name, views over every ktable with a component set (constness as access mode), the
// ktable for the writer, and commands that take effect at the phase boundary with a
// Spawned event carrying the ticket.
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

struct Pos { f32 x, y; };
struct Vel { f32 dx, dy; };
struct Hp  { i32 hp; };

constinit const FieldDesc k_pos_fields[] = {KV_FIELD(Pos, x), KV_FIELD(Pos, y)};
constinit const FieldDesc k_vel_fields[] = {KV_FIELD(Vel, dx), KV_FIELD(Vel, dy)};
constinit const FieldDesc k_hp_fields[]  = {KV_FIELD(Hp, hp)};
constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(Pos, k_pos_fields, CompPrev), KV_COMPONENT(Vel, k_vel_fields), KV_COMPONENT(Hp, k_hp_fields),
};
constinit const KTableDesc k_ktables[] = {
    {.name = "g.ship",  .components = "Pos, Vel, Hp",  .capacity = 16},
    {.name = "g.rock",  .components = "Pos, Vel",  .capacity = 16},
    {.name = "g.decal", .components = "Pos", .capacity = 16},
};

struct Trace {
    u32    ktables_seen;    // ktables the move view visited
    u32    rows_moved;
    bool   destroyed_still_valid; // right after ctx.destroy, within the phase
    bool   gone_next_phase;
    u32    spawn_ticket;
    Handle spawned;         // from the Spawned event
    u32    spawned_ktable;
    u32    hp_sum;
    Handle victim;
    Handle fresh;           // direct spawn by the writer
};
constinit Trace g_trace = {};

// Sim: moves every ktable with Pos and Vel through a view, spawns a rock directly as the
// writer of g.rock, schedules a ship by command and destroys the victim by command.
void sys_move(TickContext& ctx) {
    for (KTableRows k : ctx.view<Pos, const Vel>()) {
        ++g_trace.ktables_seen;
        Span<Pos>       pos = k.col<Pos>();
        Span<const Vel> vel = k.col<const Vel>();
        for (u32 i = 0; i < k.rows(); ++i) {
            if (!k.alive(i)) continue;
            pos[i].x += vel[i].dx;
            pos[i].y += vel[i].dy;
            ++g_trace.rows_moved;
        }
    }
    KTable* rocks = ctx.write_ktable("g.rock");
    g_trace.fresh = rocks->spawn(Pos{100, 100}, Vel{1, 0});
    g_trace.spawn_ticket = ctx.spawn_later("g.ship", Pos{7, 7}, Hp{3});
    ctx.destroy(g_trace.victim);
    g_trace.destroyed_still_valid = rocks->valid(g_trace.victim);
}

// PostSim: reads Hp by column name and the Spawned event; the victim is gone by now.
void sys_tally(TickContext& ctx) {
    const KTable* rocks = ctx.read_ktable("g.rock");
    g_trace.gone_next_phase = !rocks->valid(g_trace.victim);
    Span<const Hp> hp = ctx.read<Hp>("g.ship.Hp");
    const KTable* ships = ctx.read_ktable("g.ship");
    g_trace.hp_sum = 0;
    for (u32 i = 0; i < hp.len; ++i) if (ships->is_alive(i)) g_trace.hp_sum += (u32)hp[i].hp;
    for (const Spawned& s : ctx.received<Spawned>()) {
        if (s.ticket != g_trace.spawn_ticket) continue;
        g_trace.spawned      = s.entity;
        g_trace.spawned_ktable = s.ktable;
    }
}

// A published resource: the module owns a counter, publishes it at startup, one system
// bumps it through a write and another reads it. "g.quiet" is declared but never published.
struct Counter { u32 n; };
struct GState { Counter counter; };
constinit u32 g_counter_seen = 0;
bool g_startup(Engine& e, ModuleInstance& self) {
    GState* st = arena_push<GState>(self.arena);
    self.set_state(st);
    return engine_publish(e, self, "g.counter", &st->counter);
}
void sys_bump(TickContext& ctx) {
    static const u32 k_counter = ctx.resource_id("g.counter"); // looked up once
    ctx.resource<Counter>(k_counter)->n += 10;
}
void sys_peek(TickContext& ctx) {
    g_counter_seen = ctx.resource<const Counter>("g.counter")->n;
    KV_ASSERT(ctx.resource<const Counter>("g.quiet") == nullptr); // declared, unpublished: null and a log line
    KV_ASSERT((ctx.column_id("g.ship.Hp") & 0xFFFFFF) == engine_resource_id(*ctx.engine, "g.ship.Hp")); // tagged with the engine
    KV_ASSERT(ctx.ktable_id("g.rock") == engine_ktable_id(*ctx.engine, "g.rock"));
}

constinit const ResourceDesc k_resources[]   = {{"g.counter"}, {"g.quiet"}};
constinit const SystemDesc k_systems[] = {
    {.name = "g.move",  .phase = Phase::Sim,     .fn = sys_move,  .reads = "*.Vel",  .writes = "*.Pos, g.rock"},
    {.name = "g.tally", .phase = Phase::PostSim, .fn = sys_tally, .reads = "g.ship.Hp, g.ship, g.rock, Spawned"},
    {.name = "g.peek",  .phase = Phase::PostSim, .fn = sys_peek,  .reads = "g.counter, g.quiet"},  // after bump: it reads what bump writes
    {.name = "g.bump",  .phase = Phase::PostSim, .fn = sys_bump,  .writes = "g.counter"},
};
KV_MODULE_DEFINE(k_g) = {.name = "g", .components = k_components, .ktables = k_ktables, .resources = k_resources,
                         .systems = k_systems, .startup = g_startup, .memory = {.reserve = 4096}};

constinit const ResourceDesc k_thief_resources[] = {{"thief.own"}};
bool thief_startup(Engine& e, ModuleInstance& self) { static Counter c; return engine_publish(e, self, "g.counter", &c); }
KV_MODULE_DEFINE(k_thief) = {.name = "thief", .resources = k_thief_resources, .startup = thief_startup};

} // namespace

void test_context(kv::TestCtx& c) {
    using namespace kv;
    c.begin("context");

    const ModuleDesc* const list[] = {&k_g};
    Engine e;
    KV_CHECK(c, engine_init(e, list));

    // Resolved sets: the wildcard became one id per ktable that has the component; the
    // expanded list has the write on g.rock covering its columns, Vel included.
    const ScheduledSystem& move = e.phases.systems[(u32)Phase::Sim][0];
    KV_CHECK(c, move.declared.len == 6); // Vel in ship and rock; Pos in three ktables, plus g.rock
    // A wildcard expands in ktable order, which is by name: decal, rock, ship.
    KV_CHECK(c, move.declared[0].resource == engine_resource_id(e, "g.rock.Vel") && move.declared[0].mode == Access::Read &&
                move.declared[1].resource == engine_resource_id(e, "g.ship.Vel"));
    KV_CHECK(c, move.declared[2].resource == engine_resource_id(e, "g.decal.Pos") && move.declared[4].resource == engine_resource_id(e, "g.ship.Pos") &&
                move.declared[5].resource == engine_resource_id(e, "g.rock") &&
                move.declared[5].mode == Access::Write);
    Access rock_vel = Access::Read, ship_vel = Access::Write;
    KV_CHECK(c, move.mode(engine_resource_id(e, "g.rock.Vel"), &rock_vel) && rock_vel == Access::Write);
    KV_CHECK(c, move.mode(engine_resource_id(e, "g.ship.Vel"), &ship_vel) && ship_vel == Access::Read);
    KV_CHECK(c, !move.mode(engine_resource_id(e, "g.ship.Hp"), &ship_vel));
    KV_CHECK(c, EventType<Spawned>::id == e.event_type_count - 2 && EventType<StateEdge>::id == e.event_type_count - 1 &&
                EventType<SimReset>::id == e.event_type_count); // the core's events come last

    KTable* ship  = engine_ktable(e, "g.ship");
    KTable* rock  = engine_ktable(e, "g.rock");
    KTable* decal = engine_ktable(e, "g.decal");
    Handle s0 = ship->spawn(Pos{0, 0}, Vel{1, 1}, Hp{5});
    Handle r0 = rock->spawn(Pos{10, 0}, Vel{0, 2});
    Handle r1 = rock->spawn(Pos{20, 0}, Vel{0, 0});
    Handle d0 = decal->spawn(Pos{1, 1});
    g_trace = Trace{};
    g_trace.victim = r1;

    e.phases.run_tick(e, e.events, 1.0 / 60.0, 0);

    // The view visited ship and rock (decal has no Vel) and moved the three live rows.
    KV_CHECK(c, g_trace.ktables_seen == 2 && g_trace.rows_moved == 3);
    KV_CHECK(c, ship->get<Pos>(s0)->x == 1 && rock->get<Pos>(r0)->y == 2 && decal->get<Pos>(d0)->x == 1);
    KV_CHECK(c, ship->prev<Pos>(s0)->x == 0); // PreSim copied before the move
    // Direct spawn by the writer: there at once.
    KV_CHECK(c, rock->valid(g_trace.fresh) && rock->get<Pos>(g_trace.fresh)->x == 100);
    // Command destroy: visible within the phase, gone at the boundary.
    KV_CHECK(c, g_trace.destroyed_still_valid && g_trace.gone_next_phase && !rock->valid(r1));
    // Command spawn: applied at the boundary, Spawned delivered to the next phase, unnamed component zero.
    KV_CHECK(c, g_trace.spawn_ticket != 0 && !g_trace.spawned.is_null() && g_trace.spawned_ktable == engine_ktable_id(e, "g.ship"));
    KV_CHECK(c, ship->valid(g_trace.spawned) && ship->get<Pos>(g_trace.spawned)->x == 7 && ship->get<Hp>(g_trace.spawned)->hp == 3);
    KV_CHECK(c, ship->get<Vel>(g_trace.spawned)->dx == 0 && ship->prev<Pos>(g_trace.spawned)->x == 7);
    KV_CHECK(c, g_trace.hp_sum == 8 && ship->live() == 2);
    KV_CHECK(c, e.commands.count == 0 && e.commands.bytes.len == 0);
    // The published counter: bump ran before peek (derived from the sets) and peek saw the write.
    KV_CHECK(c, g_counter_seen == 10);
    KV_CHECK(c, e.resources[engine_resource_id(e, "g.counter") - 1].type == type_key<Counter>());

    // A tick later the Spawned event is gone and nothing is pending.
    g_trace.victim = Handle{}; // destroying null is silently nothing
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 1);
    KV_CHECK(c, ship->live() == 3 && rock->live() == 3);

    // Outside the schedule (no system) the context allows everything, for tests and tools.
    TickContext ctx{};
    ctx.engine = &e;
    ctx.events = &e.events;
    // destroy_all: a command that empties a ktable at the boundary; direct on the ktable at once.
    ctx.destroy_all("g.rock");
    KV_CHECK(c, rock->live() == 3);
    engine_apply_commands(e);
    KV_CHECK(c, rock->live() == 0 && !rock->valid(r0));
    KV_CHECK(c, ship->destroy_all() == 3 && ship->live() == 0 && ship->destroy_all() == 0);
    KV_CHECK(c, ctx.write<Hp>("g.ship.Hp").len == ship->rows() && ctx.read_ktable("g.decal") == decal);
    u32 n = 0;
    for (KTableRows k : ctx.view<const Pos>()) n += k.rows() > 0;
    KV_CHECK(c, n == 3);

    engine_shutdown(e);
    KV_CHECK(c, EventType<Spawned>::id == 0);

    // Publishing a resource another module declared fails that module's startup.
    const ModuleDesc* const thief[] = {&k_g, &k_thief};
    Engine f;
    KV_CHECK(c, !engine_init(f, thief));
}
