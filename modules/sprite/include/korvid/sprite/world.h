// Korvid sprite: the sprite world. Entities are generational slots; each component is
// its own array indexed by slot (SoA per component type). A stale handle gets null.
#pragma once
#include "korvid/core/pool.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"

namespace kv {

struct SpriteWorld {
    Pool<u8>          entities;        // liveness and generations; the payload is unused
    Array<Transform2> transforms;      // current tick, by slot
    Array<Transform2> prev_transforms; // previous tick, by slot
    Array<Sprite>     sprites;         // by slot

    Handle create(const Transform2& t, const Sprite& s);
    bool   destroy(Handle h);
    bool   valid(Handle h) const { return entities.valid(h); }
    u32    live() const { return entities.live; }

    Transform2*       transform(Handle h)       { return valid(h) ? &transforms[h.index] : nullptr; }
    const Transform2* transform(Handle h) const { return valid(h) ? &transforms[h.index] : nullptr; }
    Sprite*           sprite(Handle h)          { return valid(h) ? &sprites[h.index] : nullptr; }
    const Sprite*     sprite(Handle h)    const { return valid(h) ? &sprites[h.index] : nullptr; }

    // Start of a sim tick: the current transforms become the previous ones.
    void begin_tick();
    void free();
};

} // namespace kv
