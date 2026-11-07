#include "korvid/sprite/extract.h"
#include "korvid/sprite/components.h"
#include "korvid/sprite/world.h"

namespace kv {

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
        // The schema compares field by field, so a new Sprite field is covered the day it is declared.
        bool changed = is_new || !component_equal(sprite_schema(), &s, &world.sent_sprites[i]);
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
