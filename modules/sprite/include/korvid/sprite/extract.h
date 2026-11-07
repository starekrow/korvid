// Korvid sprite: the extract. Each tick, what changed in the sprite world becomes a run
// of deltas for the renderer's persistent world: a Tick marker, a Set for every entity
// the renderer has not seen or whose transform or sprite differs from what it was last
// told, and a Remove for every entity destroyed since. Nothing is copied twice.
#pragma once
#include "korvid/core/array.h"
#include "korvid/render/sprite.h"

namespace kv {

struct SpriteWorld;

// Appends one tick's deltas to `out`, starting with the Tick marker, and updates the
// world's record of what the renderer knows.
void extract_sprites(SpriteWorld& world, Array<SpriteDelta>& out);

} // namespace kv
