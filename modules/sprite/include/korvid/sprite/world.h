// Korvid sprite: the sprite world is the engine table of kind "sprite.entity", which the
// sprite module declares with Transform2 (prev kept) and Sprite. This is the convenience
// surface over that table: create, destroy, typed lookups. Anything a game kind with the
// same two components does renders the same way; the extract walks every such kind.
#pragma once
#include "korvid/core/kind.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"

namespace kv {

struct SpriteWorld {
    Table* table = nullptr; // the engine's; set when the module starts

    Handle create(const Transform2& t, const Sprite& s) { return table->spawn(t, s); }
    bool   destroy(Handle h) { return table->destroy(h); }
    bool   valid(Handle h) const { return table->valid(h); }
    u32    live() const { return table->live(); }
    u16    kind() const { return table->kind; }

    Transform2*       transform(Handle h)            { return table->get<Transform2>(h); }
    const Transform2* transform(Handle h) const      { return table->get<Transform2>(h); }
    const Transform2* prev_transform(Handle h) const { return table->prev<Transform2>(h); }
    Sprite*           sprite(Handle h)               { return table->get<Sprite>(h); }
    const Sprite*     sprite(Handle h) const         { return table->get<Sprite>(h); }
};

} // namespace kv
