#include "korvid/sprite/module.h"
#include "korvid/sprite/components.h"
#include "korvid/sprite/extract.h"
#include "korvid/sprite/world.h"
#include "korvid/core/engine.h"
#include "korvid/math/fields.h"

namespace kv {

namespace {

struct SpriteState {
    SpriteWorld        world;
    ExtractState       extract;
    Array<SpriteDelta> deltas;
};

bool sprite_startup(Engine& engine, ModuleInstance& self) {
    SpriteState* s = arena_push<SpriteState>(self.arena);
    if (!s) return false;
    s->deltas.alloc = s->extract.tables.alloc = &self.general;
    s->world.table = engine_table(engine, "sprite.entity"); // the engine built it from the kind table
    if (!s->world.table) return false;
    self.set_state(s);
    return true;
}

void sprite_shutdown(Engine&, ModuleInstance& self) {
    SpriteState* s = self.state_as<SpriteState>();
    if (!s) return;
    s->extract.free();
    s->deltas.free();
}

// Phase PostSim: what this tick changed, in every kind with Transform2 and Sprite,
// appended for the renderer. The engine copied current into previous at PreSim.
void sprite_extract(TickContext& ctx) {
    SpriteState& s = ctx.state_as<SpriteState>();
    extract_sprites(*ctx.engine, s.extract, s.deltas);
}

// The schemas. Transform2 keeps a previous-tick copy for interpolation; Sprite does not.
constinit const FieldDesc k_transform2_fields[] = {KV_FIELDS(Transform2, position, rotation, scale)};
constinit const FieldDesc k_sprite_fields[]     = {KV_FIELDS(Sprite, material, uv, size, tint, layer)};
constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(Transform2, k_transform2_fields, CompPrev),
    KV_COMPONENT(Sprite, k_sprite_fields),
};

// The kind. Any kind with these two components is a sprite to the extract.
constinit const char* const k_entity_components[] = {"Transform2", "Sprite"};
constinit const KindDesc    k_kinds[] = {
    {.name = "sprite.entity", .components = k_entity_components, .capacity = KV_POOL_DEFAULT_MAX},
};

constinit const ResourceDesc k_resources[] = {{"sprite.deltas"}};
constinit const char* const  k_extract_reads[]  = {"*.Transform2", "*.Sprite"};
constinit const char* const  k_extract_writes[] = {"sprite.deltas"};
constinit const SystemDesc   k_systems[]   = {
    {.name = "sprite.extract", .phase = Phase::PostSim, .fn = sprite_extract, .reads = k_extract_reads, .writes = k_extract_writes},
};

} // namespace

KV_MODULE_DEFINE(sprite_module) = {
    .name       = "sprite",
    .components = k_components,
    .kinds      = k_kinds,
    .resources  = k_resources,
    .systems    = k_systems,
    .startup    = sprite_startup,
    .shutdown   = sprite_shutdown,
    .memory     = {.reserve = 64 * 1024},
};

const ComponentDesc& transform2_schema() { return k_components[0]; }
const ComponentDesc& sprite_schema()     { return k_components[1]; }

SpriteWorld* sprite_world(Engine& e) {
    SpriteState* s = engine_module_state_as<SpriteState>(e, "sprite");
    return s ? &s->world : nullptr;
}

Array<SpriteDelta>* sprite_deltas(Engine& e) {
    SpriteState* s = engine_module_state_as<SpriteState>(e, "sprite");
    return s ? &s->deltas : nullptr;
}

} // namespace kv
