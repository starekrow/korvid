// Korvid sprite: module record. The module owns the SpriteWorld (resource
// "sprite.world") and snapshots its transforms at the start of every tick
// (system "sprite.begin_tick", phase PreSim).
#pragma once
#include "korvid/core/module.h"

namespace kv {

struct Engine;
struct SpriteWorld;

KV_MODULE_DECLARE(sprite_module);

// The sprite module's world, or null if the module is not started.
SpriteWorld* sprite_world(Engine& e);

} // namespace kv
