// Module parts: a module's tables spread over parts are joined to its own in engine_init,
// the module's first and then each part's in list order. Pinned here: ids in that order
// (ktables by name), the module's prefix on a part's names, a part using what another
// declared, the schedule's ties in joined order, the joined record the instance points at,
// the same ids again after a shutdown, the parts' startups after the module's and their
// shutdowns before it (a part's failed startup shutting down what started), and the
// rejections (a name declared twice across parts, a part listed twice, a null part).
#include "korvid/core/engine.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

struct PPos  { f32 x, y; };
struct PVel  { f32 dx, dy; };
struct PHull { u32 hp; };
struct PEvOwn  { u32 a; };
struct PEvPart { u32 b; };

char g_trace[128];
void trace(const char* s) { strncat(g_trace, s, sizeof g_trace - strlen(g_trace) - 1); }
void sys_own(TickContext&)   { trace("own "); }
void sys_ship(TickContext&)  { trace("ship "); }
void sys_boss(TickContext&)  { trace("boss "); }

// Startups and shutdowns, traced; a part's startup sees the state the module's made.
struct GameState { u32 parts_saw = 0; }; // the module of parts only has none: null
GameState g_state;
bool game_up(Engine&, ModuleInstance& self) { trace("up "); self.set_state(&g_state); return true; }
void game_down(Engine&, ModuleInstance&)    { trace("down "); }
bool ship_up(Engine&, ModuleInstance& self) { trace("ship_up "); if (GameState* g = self.state_as<GameState>()) ++g->parts_saw; return true; }
void ship_down(Engine&, ModuleInstance&)    { trace("ship_down "); }
bool boss_up(Engine&, ModuleInstance& self) { trace("boss_up "); if (GameState* g = self.state_as<GameState>()) ++g->parts_saw; return true; }
void boss_down(Engine&, ModuleInstance&)    { trace("boss_down "); }
bool fail_up(Engine&, ModuleInstance&)      { trace("fail_up "); return false; }
void fail_down(Engine&, ModuleInstance&)    { trace("fail_down "); }

// The module's own tables: one component, one ktable, one system, one event.
constinit const FieldDesc     k_pos_fields[]   = {KV_FIELD(PPos, x), KV_FIELD(PPos, y)};
constinit const ComponentDesc k_own_components[] = {KV_COMPONENT(PPos, k_pos_fields)};
constinit u32 g_world_ktable = 0;
constinit const KTableDesc    k_own_ktables[]  = {{.name = ".world", .components = "PPos", .capacity = 1, .id = &g_world_ktable}};
constinit const SystemDesc    k_own_systems[]  = {{.name = ".own", .phase = Phase::Sim, .fn = sys_own}};
constinit const EventTypeDesc k_own_events[]   = {KV_EVENT(PEvOwn)};

// A part with a component of its own and a ktable using the module's.
constinit const FieldDesc     k_vel_fields[]    = {KV_FIELD(PVel, dx), KV_FIELD(PVel, dy)};
constinit const ComponentDesc k_ship_components[] = {KV_COMPONENT(PVel, k_vel_fields)};
constinit u32 g_ship_ktable = 0;
constinit const KTableDesc    k_ship_ktables[]  = {{.name = ".ship", .components = "PPos, PVel", .capacity = 4, .id = &g_ship_ktable}};
constinit const SystemDesc    k_ship_systems[]  = {{.name = ".ship_move", .phase = Phase::Sim, .fn = sys_ship}};
constinit const EventTypeDesc k_ship_events[]   = {KV_EVENT(PEvPart)};
KV_MODULE_PART(k_ship_part) = {.events = k_ship_events, .components = k_ship_components, .ktables = k_ship_ktables, .systems = k_ship_systems,
                               .startup = ship_up, .shutdown = ship_down};

// A part using a component from the other part, and a resource.
constinit const FieldDesc     k_hull_fields[]   = {KV_FIELD(PHull, hp)};
constinit const ComponentDesc k_boss_components[] = {KV_COMPONENT(PHull, k_hull_fields)};
constinit u32 g_boss_ktable = 0;
constinit const KTableDesc    k_boss_ktables[]  = {{.name = ".boss", .components = "PPos, PVel, PHull", .capacity = 2, .id = &g_boss_ktable}};
constinit const ResourceDesc  k_boss_resources[] = {{".boss_stats"}};
constinit const SystemDesc    k_boss_systems[]  = {{.name = ".boss_think", .phase = Phase::Sim, .fn = sys_boss, .writes = ".boss_stats"}};
KV_MODULE_PART(k_boss_part) = {.components = k_boss_components, .ktables = k_boss_ktables, .resources = k_boss_resources, .systems = k_boss_systems,
                               .startup = boss_up, .shutdown = boss_down};

constinit const ModulePart* const k_parts[] = {&k_ship_part, &k_boss_part};
KV_MODULE_DEFINE(k_game) = {.name = "game", .events = k_own_events, .components = k_own_components, .ktables = k_own_ktables,
                            .systems = k_own_systems, .parts = k_parts, .startup = game_up, .shutdown = game_down};

// A part whose startup fails, after one that started.
KV_MODULE_PART(k_fail_part) = {.startup = fail_up, .shutdown = fail_down};
constinit const ModulePart* const k_failing_parts[] = {&k_ship_part, &k_fail_part, &k_boss_part};
KV_MODULE_DEFINE(k_failing) = {.name = "game", .components = k_own_components, .parts = k_failing_parts, .startup = game_up, .shutdown = game_down};

// A module made only of parts.
constinit const ModulePart* const k_only_parts[] = {&k_ship_part};
constinit const ComponentDesc k_bare_components[] = {KV_COMPONENT(PPos, k_pos_fields)};
KV_MODULE_PART(k_bare_part) = {.components = k_bare_components};
constinit const ModulePart* const k_bare_then_ship[] = {&k_bare_part, &k_ship_part};
KV_MODULE_DEFINE(k_parts_only) = {.name = "only", .parts = k_bare_then_ship};

// Rejections.
constinit const KTableDesc k_again_ktables[] = {{.name = ".ship", .components = "PPos", .capacity = 1}};
KV_MODULE_PART(k_again_part) = {.ktables = k_again_ktables};
constinit const ModulePart* const k_dup_name_parts[] = {&k_ship_part, &k_again_part};
KV_MODULE_DEFINE(k_dup_name) = {.name = "game", .components = k_own_components, .parts = k_dup_name_parts};
constinit const ModulePart* const k_twice_parts[] = {&k_ship_part, &k_ship_part};
KV_MODULE_DEFINE(k_twice) = {.name = "game", .components = k_own_components, .parts = k_twice_parts};
constinit const ModulePart* const k_null_parts[] = {&k_ship_part, nullptr};
KV_MODULE_DEFINE(k_null) = {.name = "game", .components = k_own_components, .parts = k_null_parts};

bool init_fails(TestCtx& c, const ModuleDesc& m) {
    const ModuleDesc* const list[] = {&m};
    Engine e;
    bool ok = engine_init(e, list);
    if (ok) engine_shutdown(e);
    KV_CHECK(c, e.modules.len == 0 && e.joined.len == 0 && e.joined_blocks.len == 0);
    KV_CHECK(c, ComponentType<PPos>::id == 0 && g_ship_ktable == 0);
    return !ok;
}

} // namespace

void test_parts(kv::TestCtx& c) {
    using namespace kv;
    c.begin("parts");

    const ModuleDesc* const list[] = {&k_game};
    Engine e;
    e.threads = 1; // the trace is one string: one thread, so it is the schedule's order
    g_trace[0] = 0;
    KV_CHECK(c, engine_init(e, list));
    // The module's startup, then its parts' in list order, each with the module's state.
    KV_CHECK(c, strcmp(g_trace, "up ship_up boss_up ") == 0 && g_state.parts_saw == 2);
    KV_CHECK(c, engine_module(e, "game")->parts_up == 2);

    // Ids: the module's own first, then each part's, in list order; ktables by name, so
    // where one is declared never renumbers them.
    KV_CHECK(c, EventType<PEvOwn>::id == 1 && EventType<PEvPart>::id == 2);
    KV_CHECK(c, ComponentType<PPos>::id == 1 && ComponentType<PVel>::id == 2 && ComponentType<PHull>::id == 3);
    KV_CHECK(c, g_boss_ktable == 1 && g_ship_ktable == 2 && g_world_ktable == 3);
    // A part's names take the module's prefix like its own.
    KV_CHECK(c, engine_ktable_id(e, "game.ship") == 2 && engine_ktable_id(e, "game.boss") == 1);
    KV_CHECK(c, engine_resource_id(e, "game.boss_stats") != 0);
    KV_CHECK(c, engine_system_phase(e, "game.boss_think") == Phase::Sim);
    // The boss part's ktable has the other part's component.
    KTable* boss = engine_ktable(e, "game.boss");
    KV_CHECK(c, boss && boss->has(ComponentType<PVel>::id) && boss->has(ComponentType<PHull>::id));

    // The instance holds the joined record: every table in one, no parts left.
    const ModuleDesc* d = engine_module(e, "game")->desc;
    KV_CHECK(c, d != &k_game && d->parts.len == 0 && strcmp(d->name, "game") == 0);
    KV_CHECK(c, d->ktables.len == 3 && d->components.len == 3 && d->systems.len == 3 && d->events.len == 2 && d->resources.len == 1);
    KV_CHECK(c, strcmp(d->ktables[0].name, ".world") == 0 && strcmp(d->ktables[2].name, ".boss") == 0);

    // Unordered systems run in joined declaration order.
    g_trace[0] = 0;
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 0);
    KV_CHECK(c, strcmp(g_trace, "own ship boss ") == 0);

    // The parts' shutdowns, last first, then the module's.
    g_trace[0] = 0;
    engine_shutdown(e);
    KV_CHECK(c, strcmp(g_trace, "boss_down ship_down down ") == 0);
    KV_CHECK(c, e.joined.len == 0 && e.joined_blocks.len == 0 && e.module_list.len == 0);
    KV_CHECK(c, ComponentType<PVel>::id == 0 && g_boss_ktable == 0);

    // Again, the same ids.
    e.threads = 1;
    KV_CHECK(c, engine_init(e, list));
    KV_CHECK(c, g_ship_ktable == 2 && ComponentType<PHull>::id == 3);
    engine_shutdown(e);

    // A module of parts only; a module without parts is used as given.
    const ModuleDesc* const only[] = {&k_parts_only};
    KV_CHECK(c, engine_init(e, only));
    KV_CHECK(c, g_ship_ktable == 1 && ComponentType<PPos>::id == 1 && ComponentType<PVel>::id == 2);
    KV_CHECK(c, engine_module(e, "only")->desc->ktables.len == 1);
    engine_shutdown(e);

    // A part's startup fails: init fails, and what started is shut down, the failed part
    // and those after it not.
    g_trace[0] = 0;
    KV_CHECK(c, init_fails(c, k_failing));
    KV_CHECK(c, strcmp(g_trace, "up ship_up fail_up ship_down down ") == 0);

    KV_CHECK(c, init_fails(c, k_dup_name)); // ".ship" from two parts
    KV_CHECK(c, init_fails(c, k_twice));    // the same part twice: its names twice
    KV_CHECK(c, init_fails(c, k_null));
}
