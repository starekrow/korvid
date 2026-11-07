#include "korvid/sprite/extract.h"
#include "korvid/sprite/world.h"
#include <string.h>

namespace kv {

namespace {

bool sprite_differs(const Sprite& a, const Sprite& b) {
    return a.material != b.material || a.uv.x != b.uv.x || a.uv.y != b.uv.y || a.uv.w != b.uv.w ||
           a.uv.h != b.uv.h || a.size != b.size || a.tint != b.tint || a.layer != b.layer;
}

} // namespace

void extract_sprites(SpriteWorld& world, Array<SpriteDelta>& out) {
    SpriteDelta tick{};
    tick.kind = SpriteDeltaKind::Tick;
    out.push(tick);

    for (Handle h : world.removed) {
        SpriteDelta d{};
        d.kind   = SpriteDeltaKind::Remove;
        d.entity = h;
        out.push(d);
    }
    world.removed.clear();

    u32 n = world.entities.capacity();
    for (u32 i = 0; i < n; ++i) {
        if (!world.entities.is_alive(i)) continue;
        const Sprite& s = world.sprites[i];
        bool is_new  = !world.sent[i];
        bool moved   = world.transforms[i] != world.prev_transforms[i];
        bool changed = is_new || sprite_differs(s, world.sent_sprites[i]);
        if (!is_new && !moved && !changed) continue;

        SpriteDelta d{};
        d.kind            = SpriteDeltaKind::Set;
        d.entity          = world.entities.handle_at(i);
        d.state.placement = world.transforms[i];
        d.state.material  = s.material;
        d.state.uv        = s.uv;
        d.state.size      = s.size;
        d.state.tint      = s.tint;
        d.state.layer     = s.layer;
        out.push(d);
        world.sent[i]         = 1;
        world.sent_sprites[i] = s;
    }
}

} // namespace kv
