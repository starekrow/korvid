#include "korvid/sprite/module.h"
#include "korvid/sprite/world.h"
#include "korvid/core/engine.h"
#include "korvid/core/memory.h"

namespace kv {

namespace {

bool sprite_startup(Engine&, ModuleInstance& self) {
    SpriteWorld* w = (SpriteWorld*)mem_alloc(sizeof(SpriteWorld));
    *w = SpriteWorld{};
    self.state = w;
    return true;
}

void sprite_shutdown(Engine&, ModuleInstance& self) {
    SpriteWorld* w = (SpriteWorld*)self.state;
    w->free();
    mem_free(w);
}

// Phase PreSim: the current transforms become the previous ones, before anything moves.
void sprite_begin_tick(TickContext& ctx) {
    ((SpriteWorld*)ctx.state)->begin_tick();
}

constinit const ResourceDesc k_resources[] = {{"sprite.world"}};
constinit const char* const  k_world[]     = {"sprite.world"};
constinit const SystemDesc   k_systems[]   = {
    {"sprite.begin_tick", Phase::PreSim, sprite_begin_tick, {}, k_world},
};

} // namespace

KV_MODULE_DEFINE(sprite_module) = {
    "sprite", {}, k_resources, k_systems, sprite_startup, sprite_shutdown,
};

SpriteWorld* sprite_world(Engine& e) {
    return (SpriteWorld*)engine_module_state(e, "sprite");
}

} // namespace kv
