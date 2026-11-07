// Korvid sprite: module record. The module owns the SpriteWorld (resource
// "sprite.world"), snapshots its transforms at the start of every tick
// (system "sprite.begin_tick", phase PreSim) and extracts what changed at the end of
// every tick (system "sprite.extract", phase PostSim) into a delta stream the frame
// loop hands to the renderer.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/module.h"
#include "korvid/render/sprite.h"

namespace kv {

struct Engine;
struct SpriteWorld;

KV_MODULE_DECLARE(sprite_module);

// The sprite module's world, or null if the module is not started.
SpriteWorld* sprite_world(Engine& e);
// The deltas extracted since the last take: the ticks that ran this frame, in order.
// Valid until the next tick; the caller applies them and clears.
Array<SpriteDelta>* sprite_deltas(Engine& e);

} // namespace kv
