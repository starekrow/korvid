#include "korvid/sprite/module.h"
#include "korvid/sprite/extract.h"
#include "korvid/sprite/world.h"
#include "korvid/core/engine.h"

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
    self.state = s;
    return true;
}

void sprite_shutdown(Engine&, ModuleInstance& self) {
    SpriteState* s = (SpriteState*)self.state;
    if (!s) return;
    s->world.free();
    s->deltas.free();
}

// Phase PreSim: the current transforms become the previous ones, before anything moves.
void sprite_begin_tick(TickContext& ctx) {
    ((SpriteState*)ctx.state)->world.begin_tick();
}

// Phase PostSim: what this tick changed, appended for the renderer.
void sprite_extract(TickContext& ctx) {
    SpriteState& s = *(SpriteState*)ctx.state;
    extract_sprites(s.world, s.deltas);
}

constinit const ResourceDesc k_resources[] = {{"sprite.world"}, {"sprite.deltas"}};
constinit const char* const  k_world[]     = {"sprite.world"};
constinit const char* const  k_deltas[]    = {"sprite.deltas"};
constinit const SystemDesc   k_systems[]   = {
    {"sprite.begin_tick", Phase::PreSim,  sprite_begin_tick, {}, k_world},
    {"sprite.extract",    Phase::PostSim, sprite_extract, k_world, k_deltas},
};

} // namespace

KV_MODULE_DEFINE(sprite_module) = {
    "sprite", {}, k_resources, k_systems, sprite_startup, sprite_shutdown,
    {.reserve = 64 * 1024},
};

SpriteWorld* sprite_world(Engine& e) {
    SpriteState* s = (SpriteState*)engine_module_state(e, "sprite");
    return s ? &s->world : nullptr;
}

Array<SpriteDelta>* sprite_deltas(Engine& e) {
    SpriteState* s = (SpriteState*)engine_module_state(e, "sprite");
    return s ? &s->deltas : nullptr;
}

} // namespace kv
