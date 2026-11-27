// What a handler receives does not depend on the thread count when its events name entities
// another unit of its level destroys: every unit of a level sees who was alive when the level
// began (prepare_unit, phase.cc). Before that, one thread and eight played different games.
//
//
// A system in PreSim emits a Poke for every thing. In Sim, `cull` destroys every thing
// (it holds the ktable's write) and `on_poke` counts the Pokes it is handed into a ktable
// of its own. Neither declares anything the other touches, so they share a level. The bus
// drops an event whose handle is stale when the handler's batch is made, before the level
// runs: every thing is still alive then, so every Poke arrives, on any thread count.
#include "korvid/core/engine.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"
#include "korvid/core/log.h"
#include <string.h>

namespace {

using namespace kv;

constexpr u32 kThings = 20000, kTicks = 20;

struct Tag { u32 n; };
struct Count { u64 n; };
constinit const FieldDesc k_tag_fields[] = {KV_FIELDS(Tag, n)};
constinit const FieldDesc k_count_fields[] = {KV_FIELDS(Count, n)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Tag, k_tag_fields), KV_COMPONENT(Count, k_count_fields)};
constinit u32 g_thing = 0;
constinit const KTableDesc k_ktables[] = {
    {.name = "sr.thing", .components = "Tag", .capacity = 2 * kThings, .id = &g_thing},
    {.name = "sr.tally", .components = "Count", .capacity = 4},
};
struct Poke { Handle who; };
constinit const FieldDesc k_poke_fields[] = {KV_FIELDS(Poke, who)};
constinit const EventTypeDesc k_events[] = {KV_EVENT_EX(Poke, k_poke_fields, EvStaleOk)};

void emit_pokes(TickContext& ctx) {
    for (KTableRows k : ctx.view<const Tag>())
        for (u32 i = 0; i < k.rows(); ++i)
            if (k.alive(i)) ctx.emit(Poke{k.handle_at(i)});
}
void cull(TickContext& ctx) {
    KTable* t = ctx.write_ktable(g_thing);
    for (u32 i = 0, n = t->rows(); i < n; ++i)
        if (t->is_alive(i)) t->destroy(t->handle_at(i));
    for (u32 i = 0; i < kThings; ++i) t->spawn(Tag{i});
}
void on_poke(TickContext& ctx, Span<const Poke> pokes) {
    u32 k_count = ctx.column_id("sr.tally.Count"); // looked up each time: this test makes many engines
    ctx.write<Count>(k_count)[0].n += pokes.len;
}
bool startup(Engine& e, ModuleInstance&) {
    KTable* t = engine_ktable(e, g_thing);
    for (u32 i = 0; i < kThings; ++i) t->spawn(Tag{i});
    engine_ktable(e, "sr.tally")->spawn(Count{0});
    return true;
}

constinit const SystemDesc k_systems[] = {
    {.name = "sr.emit", .phase = Phase::PreSim, .fn = emit_pokes, .reads = "sr.thing.Tag", .emits = "Poke", .state = StateUse::None},
    {.name = "sr.cull", .phase = Phase::Sim, .fn = cull, .writes = "sr.thing", .state = StateUse::None},
};
constinit const HandlerDesc k_main_handlers[] = {{.name = "sr.on_poke", KV_ON(Poke, on_poke), .phase = Phase::Sim, .writes = "sr.tally.Count"}};
constinit const HandlerDesc k_any_handlers[] = {{.name = "sr.on_poke", KV_ON(Poke, on_poke), .phase = Phase::Sim, .writes = "sr.tally.Count", .any_thread = true}};
KV_MODULE_DEFINE(k_sr_main) = {.name = "sr", .events = k_events, .components = k_components, .ktables = k_ktables, .systems = k_systems, .handlers = k_main_handlers, .startup = startup};
KV_MODULE_DEFINE(k_sr_any) = {.name = "sr", .events = k_events, .components = k_components, .ktables = k_ktables, .systems = k_systems, .handlers = k_any_handlers, .startup = startup};

struct Outcome { bool ok; u64 received; u64 hash; bool same_level; };
Outcome run(const ModuleDesc* m, u32 threads) {
    Outcome o{};
    const ModuleDesc* const list[] = {m};
    Engine e;
    e.threads = threads;
    LogLevel was = log_level();
    log_set_level(LogLevel::Warn);
    o.ok = engine_init(e, list);
    if (o.ok) {
        u32 cull_level = 0, poke_level = 0;
        for (const ScheduledSystem& s : e.phases.systems[(u32)Phase::Sim]) {
            if (strcmp(s.name(), "sr.cull") == 0) cull_level = s.level;
            if (strcmp(s.name(), "sr.on_poke") == 0) poke_level = s.level;
        }
        o.same_level = cull_level && cull_level == poke_level;
        for (u64 t = 0; t < kTicks; ++t) e.phases.run_tick(e, e.events, 1.0 / 60.0, t);
        o.received = engine_ktable(e, "sr.tally")->rows_of<Count>()[0].n;
        o.hash = engine_hash_state(e);
    }
    engine_shutdown(e);
    log_set_level(was);
    return o;
}

} // namespace

void test_stale_order(kv::TestCtx& c) {
    using namespace kv;
    c.begin("stale_order");
    const ModuleDesc* variants[] = {&k_sr_main, &k_sr_any};

    for (u32 v = 0; v < 2; ++v) {
        Outcome one = run(variants[v], 1);
        KV_CHECK(c, one.ok && one.same_level && one.received == (u64)kThings * kTicks); // the pair share a level, and nothing was dropped
        for (u32 r = 0; r < 6; ++r) {
            Outcome many = run(variants[v], 8);
            KV_CHECK(c, many.ok && many.received == one.received && many.hash == one.hash);
        }
    }
}
