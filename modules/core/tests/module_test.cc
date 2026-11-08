// The registry's ordering rules, each pinned by a check:
// ids in list-then-declaration order, schedule in phase/module/declaration order,
// startup in list order and shutdown in reverse, rollback on failure, and the four
// rejections (duplicate module, event, system, resource; unknown resource reference).
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

struct EvA { u32 a; };
struct EvB { u32 b; };
struct EvC { u32 c; };

// Shared trace of what ran, in order. Plain static storage, zero-initialized.
char g_trace[256];
void trace(const char* s) { strncat(g_trace, s, sizeof g_trace - strlen(g_trace) - 1); }

void sys_first_input(TickContext&)  { trace("1i "); }
void sys_first_sim(TickContext&)    { trace("1s "); }
void sys_second_input(TickContext&) { trace("2i "); }
void sys_second_sim_a(TickContext&) { trace("2sa "); }
struct SecondState { u32 ticks; u16 owner_seen; void* scratch_block; };

void sys_second_sim_b(TickContext& ctx) {
    trace("2sb ");
    SecondState* s = &ctx.state_as<SecondState>(); // the system gets its own module's state, checked
    s->ticks += 1;
    s->owner_seen = mem_owner();              // attribution follows the schedule
    s->scratch_block = mem_alloc(64);         // tagged to module 2, freed in shutdown
}

bool first_startup(Engine&, ModuleInstance&)  { trace("up1 "); return true; }
void first_shutdown(Engine&, ModuleInstance&) { trace("down1 "); }
bool second_startup(Engine&, ModuleInstance& self) {
    trace("up2 ");
    self.set_state(arena_push<SecondState>(self.arena)); // declared memory: the arena exists
    return self.state != nullptr;
}
void second_shutdown(Engine&, ModuleInstance& self) {
    trace("down2 ");
    mem_free(self.state_as<SecondState>()->scratch_block);
}
bool failing_startup(Engine&, ModuleInstance&) { trace("upfail "); return false; }

constinit const EventTypeDesc k_first_events[]  = {KV_EVENT(EvA), KV_EVENT(EvB)};
constinit const EventTypeDesc k_second_events[] = {KV_EVENT(EvC)};
constinit const ResourceDesc  k_first_res[]     = {{"first.store"}};
constinit const ResourceDesc  k_second_res[]    = {{"second.store"}};

constinit const SystemDesc k_first_systems[] = {
    {.name = "first.sim",   .phase = Phase::Sim,   .fn = sys_first_sim},   // declared before the Input one on purpose
    {.name = "first.input", .phase = Phase::Input, .fn = sys_first_input},
};
constinit const SystemDesc k_second_systems[] = {
    {.name = "second.sim_a", .phase = Phase::Sim,   .fn = sys_second_sim_a},
    {.name = "second.input", .phase = Phase::Input, .fn = sys_second_input},
    {.name = "second.sim_b", .phase = Phase::Sim,   .fn = sys_second_sim_b, .reads = "first.store", .writes = "second.store"},
};

KV_MODULE_DEFINE(k_first)  = {.name = "first", .events = k_first_events, .resources = k_first_res,
                              .systems = k_first_systems, .startup = first_startup, .shutdown = first_shutdown};
KV_MODULE_DEFINE(k_second) = {.name = "second", .events = k_second_events, .resources = k_second_res,
                              .systems = k_second_systems, .startup = second_startup, .shutdown = second_shutdown,
                              .memory = {.reserve = 1u << 20}};
KV_MODULE_DEFINE(k_failing) = {.name = "failing", .startup = failing_startup};
KV_MODULE_DEFINE(k_first_again) = {.name = "first"};

constinit const EventTypeDesc k_dup_events[] = {KV_EVENT(EvA)};
KV_MODULE_DEFINE(k_dup_event) = {.name = "dupevent", .events = k_dup_events};

constinit const SystemDesc k_dup_systems[] = {{.name = "first.sim", .phase = Phase::Sim, .fn = sys_first_sim}};
KV_MODULE_DEFINE(k_dup_system) = {.name = "dupsystem", .systems = k_dup_systems};

constinit const ResourceDesc k_dup_res[] = {{"first.store"}};
KV_MODULE_DEFINE(k_dup_resource) = {.name = "dupresource", .resources = k_dup_res};

constinit const SystemDesc k_bad_ref_systems[] = {{.name = "bad.sim", .phase = Phase::Sim, .fn = sys_first_sim, .reads = "nowhere.store"}};
KV_MODULE_DEFINE(k_bad_ref) = {.name = "badref", .systems = k_bad_ref_systems};

// More modules than memory accounting can attribute: rejected before anything starts.
constinit const ModuleDesc k_many[65] = {{.name = "m0"}, {.name = "m1"}, {.name = "m2"}, {.name = "m3"}, {.name = "m4"}, {.name = "m5"}, {.name = "m6"}, {.name = "m7"}, {.name = "m8"}, {.name = "m9"}, {.name = "m10"}, {.name = "m11"}, {.name = "m12"}, {.name = "m13"}, {.name = "m14"}, {.name = "m15"}, {.name = "m16"}, {.name = "m17"}, {.name = "m18"}, {.name = "m19"}, {.name = "m20"}, {.name = "m21"}, {.name = "m22"}, {.name = "m23"}, {.name = "m24"}, {.name = "m25"}, {.name = "m26"}, {.name = "m27"}, {.name = "m28"}, {.name = "m29"}, {.name = "m30"}, {.name = "m31"}, {.name = "m32"}, {.name = "m33"}, {.name = "m34"}, {.name = "m35"}, {.name = "m36"}, {.name = "m37"}, {.name = "m38"}, {.name = "m39"}, {.name = "m40"}, {.name = "m41"}, {.name = "m42"}, {.name = "m43"}, {.name = "m44"}, {.name = "m45"}, {.name = "m46"}, {.name = "m47"}, {.name = "m48"}, {.name = "m49"}, {.name = "m50"}, {.name = "m51"}, {.name = "m52"}, {.name = "m53"}, {.name = "m54"}, {.name = "m55"}, {.name = "m56"}, {.name = "m57"}, {.name = "m58"}, {.name = "m59"}, {.name = "m60"}, {.name = "m61"}, {.name = "m62"}, {.name = "m63"}, {.name = "m64"}};

bool init_fails(TestCtx& c, Span<const ModuleDesc* const> list) {
    Engine e;
    bool ok = engine_init(e, list);
    if (ok) engine_shutdown(e);
    // Whatever happened, ids are back to 0 and nothing is left started.
    KV_CHECK(c, EventType<EvA>::id == 0 && EventType<EvC>::id == 0);
    KV_CHECK(c, e.modules.len == 0);
    return !ok;
}

} // namespace

void test_module(kv::TestCtx& c) {
    using namespace kv;
    c.begin("module");

    const ModuleDesc* const list[] = {&k_first, &k_second};
    Engine e;
    e.threads = 1; // the trace below is a global string: one thread, so the order is the schedule's
    g_trace[0] = 0;
    KV_CHECK(c, engine_init(e, list));

    // Event ids: module order, then declaration order, from 1.
    KV_CHECK(c, EventType<EvA>::id == 1 && EventType<EvB>::id == 2 && EventType<EvC>::id == 3);
    KV_CHECK(c, e.event_type_count == 5); // plus the engine's own Spawned and StateEdge, registered last

    // Resources likewise, after the two modules' implicit state resources.
    KV_CHECK(c, engine_resource_id(e, "first.state") == 1 && engine_resource_id(e, "second.state") == 2);
    KV_CHECK(c, engine_resource_id(e, "first.store") == 3);
    KV_CHECK(c, engine_resource_id(e, "second.store") == 4);
    KV_CHECK(c, engine_resource_id(e, "nope") == 0);

    // Startup in list order; state is where the module put it.
    KV_CHECK(c, strcmp(g_trace, "up1 up2 ") == 0);
    KV_CHECK(c, engine_module(e, "second") && engine_module(e, "second")->index == 1);
    KV_CHECK(c, engine_module_state(e, "first") == nullptr);
    KV_CHECK(c, engine_module_state(e, "second") != nullptr);
    KV_CHECK(c, engine_module(e, "absent") == nullptr);

    // Schedule: phase order, then module order, then declaration order.
    KV_CHECK(c, e.phases.system_count() == 5);
    g_trace[0] = 0;
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 0);
    KV_CHECK(c, strcmp(g_trace, "1i 2i 1s 2sa 2sb ") == 0);
    SecondState* second = engine_module_state_as<SecondState>(e, "second");
    KV_CHECK(c, second->ticks == 1);

    // Memory: the module without a memory table has no arena; the one with it has its
    // state inside the arena, and what its system allocated is attributed to it.
    KV_CHECK(c, engine_module(e, "first")->arena.base == nullptr);
    Arena& arena = engine_module(e, "second")->arena;
    KV_CHECK(c, arena.base && (u8*)second >= arena.base && (u8*)second < arena.base + arena.used);
    KV_CHECK(c, arena.a.tag.module == 2 && arena.a.tag.category == 0 && arena.a.tag.heap == 0);
    KV_CHECK(c, second->owner_seen == 2);
#if KV_DEV
    KV_CHECK(c, mem_stats_module(2).committed_bytes == arena.committed);
    KV_CHECK(c, mem_stats_module(2).live_allocs == 1 && mem_stats_module(2).live_bytes == 64);
#endif
    KV_CHECK(c, mem_owner() == 0); // reset after the tick

    // Events registered through the list work on the engine's bus.
    e.events.emit(EvC{7});
    e.events.deliver();
    KV_CHECK(c, e.events.read<EvC>().len == 1 && e.events.read<EvC>()[0].c == 7);
    e.events.end_tick();

    // Shutdown in reverse; ids cleared.
    g_trace[0] = 0;
    engine_shutdown(e);
    KV_CHECK(c, strcmp(g_trace, "down2 down1 ") == 0);
    KV_CHECK(c, EventType<EvA>::id == 0 && EventType<EvC>::id == 0);
    KV_CHECK(c, e.modules.len == 0 && e.phases.system_count() == 0);
#if KV_DEV
    KV_CHECK(c, mem_stats_module(2).live_allocs == 0);
    KV_CHECK(c, mem_stats_module(2).committed_bytes == 0); // the arena went with the module
#endif

    // The same list initializes again with the same ids: deterministic.
    e.threads = 1;
    KV_CHECK(c, engine_init(e, list));
    KV_CHECK(c, EventType<EvC>::id == 3);
    engine_shutdown(e);

    // A failing startup rolls back what started, in reverse, and reports failure.
    const ModuleDesc* const fail_list[] = {&k_first, &k_failing, &k_second};
    g_trace[0] = 0;
    KV_CHECK(c, init_fails(c, fail_list));
    KV_CHECK(c, strcmp(g_trace, "up1 upfail down1 ") == 0);

    // Rejections.
    const ModuleDesc* const dup_module[]   = {&k_first, &k_first_again};
    const ModuleDesc* const dup_event[]    = {&k_first, &k_dup_event};
    const ModuleDesc* const dup_system[]   = {&k_first, &k_dup_system};
    const ModuleDesc* const dup_resource[] = {&k_first, &k_dup_resource};
    const ModuleDesc* const bad_ref[]      = {&k_first, &k_bad_ref};
    KV_CHECK(c, init_fails(c, dup_module));
    KV_CHECK(c, init_fails(c, dup_event));
    KV_CHECK(c, init_fails(c, dup_system));
    KV_CHECK(c, init_fails(c, dup_resource));
    KV_CHECK(c, init_fails(c, bad_ref));

    const ModuleDesc* many[65];
    for (u32 i = 0; i < 65; ++i) many[i] = &k_many[i];
    KV_CHECK(c, init_fails(c, Span<const ModuleDesc* const>(many, 65)));
    KV_CHECK(c, !init_fails(c, Span<const ModuleDesc* const>(many, 64)));
}
