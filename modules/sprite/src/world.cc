#include "korvid/sprite/world.h"
#include <stdio.h>

namespace kv {

namespace {

// What a blind resolve of a sprite entity means: its transform.
void* sprite_resolve(kv_handle h, void* ctx) {
    return ((SpriteWorld*)ctx)->transform(Handle{h});
}

int sprite_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    SpriteWorld& w = *(SpriteWorld*)ctx;
    Handle hh{h};
    const Transform2* t = w.transform(hh);
    const Sprite* s = w.sprite(hh);
    if (!t || !s) return snprintf(out, cap, "%s (%u live)", hh.index() < w.entities.len ? "stale" : "out of range", w.live());
    return snprintf(out, cap, "at (%.1f, %.1f) layer %d (%u live)", (double)t->position.x, (double)t->position.y, s->layer, w.live());
}

} // namespace

bool SpriteWorld::init(Allocator* columns, u32 max_entities, MemTag pool_tag) {
    transforms.alloc = prev_transforms.alloc = sprites.alloc = columns;
    kv_handle_kind_desc desc{};
    desc.name     = "sprite.entity";
    desc.type_key = type_key<Transform2>();
    desc.ctx      = this;
    desc.resolve  = sprite_resolve;
    desc.describe = sprite_describe;
    desc.flags    = KV_HANDLE_GENERATIONAL;
    kind = handle_kind_register(desc);
    if (!kind) return false;
    entities.use_kind(kind);
    return entities.init(max_entities, pool_tag);
}

Handle SpriteWorld::create(const Transform2& t, const Sprite& s) {
    Handle h = entities.create(0);
    if (h.is_null()) return h;
    u32 i = h.index();
    // The pool grows the slot arrays in step: a fresh slot is always at the end.
    if (i >= transforms.len) {
        KV_ASSERT(i == transforms.len);
        transforms.push(t);
        prev_transforms.push(t);
        sprites.push(s);
    } else {
        transforms[i]      = t;
        prev_transforms[i] = t; // a new entity has no history: no interpolation jump
        sprites[i]         = s;
    }
    return h;
}

bool SpriteWorld::destroy(Handle h) { return entities.destroy(h); }

void SpriteWorld::begin_tick() {
    u32 n = entities.capacity();
    for (u32 i = 0; i < n; ++i)
        if (entities.is_alive(i)) prev_transforms[i] = transforms[i];
}

void SpriteWorld::free() {
    if (kind) kv_handle_kind_unregister(kind);
    kind = 0;
    entities.free();
    transforms.free();
    prev_transforms.free();
    sprites.free();
}

} // namespace kv
