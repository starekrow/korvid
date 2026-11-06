#include "korvid/sprite/extract.h"
#include "korvid/sprite/world.h"

namespace kv {

void extract_sprites(const SpriteWorld& world, Array<RenderPacket>& out) {
    out.clear();
    u32 n = world.entities.capacity();
    for (u32 i = 0; i < n; ++i) {
        if (!world.entities.alive[i]) continue;
        const Sprite& s = world.sprites[i];
        RenderPacket p;
        p.entity   = Handle{i, world.entities.gens[i]};
        p.prev     = world.prev_transforms[i];
        p.cur      = world.transforms[i];
        p.material = s.material;
        p.uv       = s.uv;
        p.size     = s.size;
        p.tint     = s.tint;
        p.layer    = s.layer;
        out.push(p);
    }
}

} // namespace kv
