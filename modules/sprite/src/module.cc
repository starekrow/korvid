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
    Array<SpriteDelta> deltas;
};

bool sprite_startup(Engine&, ModuleInstance& self) {
    SpriteState* s = arena_push<SpriteState>(self.arena);
    if (!s) return false;
    s->deltas.alloc = &self.general;
    if (!s->world.init(&self.general, KV_POOL_DEFAULT_MAX, self.general.tag)) return false;
    self.set_state(s);
    return true;
}

void sprite_shutdown(Engine&, ModuleInstance& self) {
    SpriteState* s = self.state_as<SpriteState>();
    if (!s) return;
    s->world.free();
    s->deltas.free();
}

// Phase PreSim: the current transforms become the previous ones, before anything moves.
void sprite_begin_tick(TickContext& ctx) {
    ctx.state_as<SpriteState>().world.begin_tick();
}

// Phase PostSim: what this tick changed, appended for the renderer.
void sprite_extract(TickContext& ctx) {
    SpriteState& s = ctx.state_as<SpriteState>();
    extract_sprites(s.world, s.deltas);
}

// The schemas. Transform2 keeps a previous-tick copy for interpolation; Sprite does not.
constinit const FieldDesc k_transform2_fields[] = {
    KV_FIELD(Transform2, position), KV_FIELD(Transform2, rotation), KV_FIELD(Transform2, scale),
};
constinit const FieldDesc k_sprite_fields[] = {
    KV_FIELD(Sprite, material), KV_FIELD(Sprite, uv), KV_FIELD(Sprite, size), KV_FIELD(Sprite, tint), KV_FIELD(Sprite, layer),
};
constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(Transform2, k_transform2_fields, CompPrev),
    KV_COMPONENT(Sprite, k_sprite_fields),
};

constinit const ResourceDesc k_resources[] = {{"sprite.world"}, {"sprite.deltas"}};
constinit const char* const  k_world[]     = {"sprite.world"};
constinit const char* const  k_deltas[]    = {"sprite.deltas"};
constinit const SystemDesc   k_systems[]   = {
    {.name = "sprite.begin_tick", .phase = Phase::PreSim,  .fn = sprite_begin_tick, .writes = k_world},
    {.name = "sprite.extract",    .phase = Phase::PostSim, .fn = sprite_extract, .reads = k_world, .writes = k_deltas},
};

} // namespace

KV_MODULE_DEFINE(sprite_module) = {
    .name       = "sprite",
    .components = k_components,
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
