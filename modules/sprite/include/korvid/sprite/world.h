// Korvid sprite: the sprite world. Entities are generational slots; each component is
// its own array indexed by slot (SoA per component type). A stale handle gets null.
// The world is the handle issuer: init registers the kind "sprite.entity", whose blind
// resolve is the entity's Transform2 and whose description is its position and layer.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/pool.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"

namespace kv {

struct SpriteWorld {
    Pool<u8>          entities;        // liveness and generations; the payload is unused
    Array<Transform2> transforms;      // current tick, by slot
    Array<Transform2> prev_transforms; // previous tick, by slot
    Array<Sprite>     sprites;         // by slot
    u16               kind = 0;        // the registered handle kind; 0 until init

    // Extract bookkeeping: what the renderer has been told, so a tick sends only changes.
    Array<u8>         sent;            // by slot: the renderer knows this entity
    Array<Sprite>     sent_sprites;    // by slot: the Sprite the renderer has
    Array<Handle>     removed;         // destroyed since the last extract, still to be removed

    // Registers the handle kind, says who the storage is attributed to and how many
    // slots to reserve. Without it, handles are local (kind 0) and storage is default.
    bool init(Allocator* columns, u32 max_entities, MemTag pool_tag);

    Handle create(const Transform2& t, const Sprite& s);
    bool   destroy(Handle h); // queues a Remove for the next extract
    bool   valid(Handle h) const { return entities.valid(h); }
    u32    live() const { return entities.live; }

    Transform2*       transform(Handle h)       { return valid(h) ? &transforms[h.index()] : nullptr; }
    const Transform2* transform(Handle h) const { return valid(h) ? &transforms[h.index()] : nullptr; }
    Sprite*           sprite(Handle h)          { return valid(h) ? &sprites[h.index()] : nullptr; }
    const Sprite*     sprite(Handle h)    const { return valid(h) ? &sprites[h.index()] : nullptr; }

    // Start of a sim tick: the current transforms become the previous ones.
    void begin_tick();
    void free();
};

} // namespace kv
