// Korvid sprite: module record. The module declares the Transform2 and Sprite schemas
// and the kind "sprite.entity" (resources "sprite.entity", "sprite.entity.Transform2",
// "sprite.entity.Sprite"), and extracts what changed in every kind with those two
// components at the end of every tick (system "sprite.extract", phase PostSim) into a
// delta stream the frame loop hands to the renderer.
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
// How many sprite rows the game expects (the frame loop passes FrameLoopConfig::sprite_rows):
// the extract sizes its tables to it once instead of growing them mid-run. See ExtractState.
void sprite_rows_hint(Engine& e, u32 rows);

} // namespace kv
