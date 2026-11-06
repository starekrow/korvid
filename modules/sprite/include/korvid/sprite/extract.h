// Korvid sprite: the extract. After the last sim tick of a frame, every live sprite
// entity becomes one render packet. A full copy per frame; deltas come later.
#pragma once
#include "korvid/core/array.h"
#include "korvid/render/packet.h"

namespace kv {

struct SpriteWorld;

// Clears out, then appends one packet per live entity in slot order.
void extract_sprites(const SpriteWorld& world, Array<RenderPacket>& out);

} // namespace kv
