// Entity names: unique among the living across ktables, set now from outside the schedule or
// at the boundary by command, found by name and by handle, part of the hash, gone with the
// entity, and answered through the handle registry (kv_handle_name, kv_handle_find).
#include "korvid/core/engine.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

struct Spot { f32 x; };
constinit const FieldDesc k_spot_fields[] = {KV_FIELD(Spot, x)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Spot, k_spot_fields)};
constinit const KTableDesc k_ktables[] = {
    {.name = "n.villager", .components = "Spot", .capacity = 8},
    {.name = "n.rock",     .components = "Spot", .capacity = 8},
};

Handle g_named_by_command;
u32    g_ticket = 0;

// Sim: names one villager by command and spawns a named rock; the names land at the boundary.
void sys_name(TickContext& ctx) {
    const KTable* v = ctx.read_ktable("n.villager");
    for (u32 i = 0; i < v->rows(); ++i)
        if (v->is_alive(i) && !engine_entity_name(*ctx.engine, v->handle_at(i))) { ctx.name_later(v->handle_at(i), "baker"); break; }
    g_named_by_command = engine_entity_find(*ctx.engine, "baker"); // not yet
    g_ticket = ctx.spawn_named_later("n.rock", "boulder", Spot{3.0f});
}

constinit const SystemDesc k_systems[] = {
    {.name = "n.name", .phase = Phase::Sim, .fn = sys_name, .reads = "n.villager", .writes = "n.rock"},
};
KV_MODULE_DEFINE(k_n) = {.name = "n", .components = k_components, .ktables = k_ktables, .systems = k_systems};

} // namespace

void test_entity_name(kv::TestCtx& c) {
    using namespace kv;
    c.begin("entity names");
    const ModuleDesc* const list[] = {&k_n};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KTable* villagers = engine_ktable(e, "n.villager");
    KTable* rocks = engine_ktable(e, "n.rock");
    Handle old_man = villagers->spawn(Spot{1.0f});
    Handle smith = villagers->spawn(Spot{2.0f});
    Handle pebble = rocks->spawn(Spot{0.0f});

    u64 before = rocks->hash();
    KV_CHECK(c, engine_entity_name_set(e, old_man, "old_man"));
    KV_CHECK(c, engine_entity_find(e, "old_man") == old_man);
    KV_CHECK(c, strcmp(engine_entity_name(e, old_man), "old_man") == 0 && engine_entity_name(e, smith) == nullptr);
    // Unique across ktables: a rock cannot take a villager's name; the villager keeps it.
    KV_CHECK(c, !engine_entity_name_set(e, pebble, "old_man"));
    KV_CHECK(c, engine_entity_find(e, "old_man") == old_man && engine_entity_name(e, pebble) == nullptr);
    // Through the registry, by kind.
    KV_CHECK(c, strcmp(handle_name(old_man), "old_man") == 0 && handle_name(smith) == nullptr);
    KV_CHECK(c, handle_find(old_man.kind(), "old_man") == old_man && handle_find(pebble.kind(), "old_man").is_null());
    KV_CHECK(c, kv_handle_kind_names(old_man.kind()) != 0);
    // A rename frees the old name; the hash of a ktable follows its names.
    KV_CHECK(c, engine_entity_name_set(e, pebble, "pebble") && rocks->hash() != before);
    KV_CHECK(c, engine_entity_name_set(e, pebble, "stone") && engine_entity_find(e, "pebble").is_null() && engine_entity_find(e, "stone") == pebble);
    KV_CHECK(c, engine_entity_name_set(e, pebble, "") && engine_entity_name(e, pebble) == nullptr && rocks->hash() == before);
    KV_CHECK(c, engine_entity_find(e, "stone").is_null());

    // By command: the name lands at the boundary, with a spawn or on a living entity.
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 0);
    KV_CHECK(c, g_named_by_command.is_null());
    KV_CHECK(c, engine_entity_find(e, "baker") == smith);
    Handle boulder = engine_entity_find(e, "boulder");
    KV_CHECK(c, !boulder.is_null() && rocks->get<Spot>(boulder)->x == 3.0f);
    // The second tick asks for "boulder" again: taken, refused, and the first keeps it.
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 1);
    KV_CHECK(c, engine_entity_find(e, "boulder") == boulder && rocks->live() == 3);

    // Death: the name stops resolving at once (the handle is stale), and the slot's next
    // entity does not inherit it.
    rocks->destroy(boulder);
    KV_CHECK(c, engine_entity_find(e, "boulder").is_null() && handle_name(boulder) == nullptr);
    engine_apply_commands(e); // the sweep
    KV_CHECK(c, rocks->names.count() == 0 && !e.entity_names.has(name_lookup("boulder").hash));
    Handle again = rocks->spawn(Spot{9.0f});
    KV_CHECK(c, engine_entity_name(e, again) == nullptr);
    // And the name is free for anyone.
    KV_CHECK(c, engine_entity_name_set(e, again, "boulder") && engine_entity_find(e, "boulder") == again);

    u32 seen = 0;
    engine_entity_names_each(e, [](Handle, Name, void* u) { ++*(u32*)u; return true; }, &seen);
    KV_CHECK(c, seen == 3); // old_man, baker, boulder

    engine_shutdown(e);
}
