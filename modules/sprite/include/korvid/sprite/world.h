// Korvid sprite: the sprite world is the engine's ktable "sprite.entity", which the
// sprite module declares with Transform2 (prev kept) and Sprite. This is the convenience
// surface over that ktable: create, destroy, typed lookups. Anything a game ktable with the
// same two components does renders the same way; the extract walks every such ktable.
#pragma once
#include "korvid/core/ktable.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"

namespace kv {

struct SpriteWorld {
    KTable* ktable = nullptr; // the engine's; set when the module starts

    Handle create(const Transform2& t, const Sprite& s) { return ktable->spawn(t, s); }
    bool   destroy(Handle h) { return ktable->destroy(h); }
    bool   valid(Handle h) const { return ktable->valid(h); }
    u32    live() const { return ktable->live(); }
    u16    kind() const { return ktable->handle_kind; }

    Transform2*       transform(Handle h)            { return ktable->get<Transform2>(h); }
    const Transform2* transform(Handle h) const      { return ktable->get<Transform2>(h); }
    const Transform2* prev_transform(Handle h) const { return ktable->prev<Transform2>(h); }
    Sprite*           sprite(Handle h)               { return ktable->get<Sprite>(h); }
    const Sprite*     sprite(Handle h) const         { return ktable->get<Sprite>(h); }
};

} // namespace kv
