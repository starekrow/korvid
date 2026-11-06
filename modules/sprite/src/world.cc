#include "korvid/sprite/world.h"

namespace kv {

Handle SpriteWorld::create(const Transform2& t, const Sprite& s) {
    Handle h = entities.create(0);
    // The pool grows the slot arrays in step: a fresh slot is always at the end.
    if (h.index >= transforms.len) {
        KV_ASSERT(h.index == transforms.len);
        transforms.push(t);
        prev_transforms.push(t);
        sprites.push(s);
    } else {
        transforms[h.index]      = t;
        prev_transforms[h.index] = t; // a new entity has no history: no interpolation jump
        sprites[h.index]         = s;
    }
    return h;
}

bool SpriteWorld::destroy(Handle h) { return entities.destroy(h); }

void SpriteWorld::begin_tick() {
    u32 n = entities.capacity();
    for (u32 i = 0; i < n; ++i)
        if (entities.alive[i]) prev_transforms[i] = transforms[i];
}

void SpriteWorld::free() {
    entities.free();
    transforms.free();
    prev_transforms.free();
    sprites.free();
}

} // namespace kv
