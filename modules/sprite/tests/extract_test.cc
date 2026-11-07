#include "korvid/sprite/extract.h"
#include "korvid/sprite/world.h"
#include "korvid/core/test.h"

static kv::u32 count_kind(const kv::Array<kv::SpriteDelta>& d, kv::SpriteDeltaKind k) {
    kv::u32 n = 0;
    for (const kv::SpriteDelta& x : d) n += x.kind == k;
    return n;
}

void test_extract(kv::TestCtx& c) {
    using namespace kv;
    c.begin("extract");

    SpriteWorld w;
    Sprite s0; s0.layer = 1; s0.tint = color_red(); s0.size = vec2{32, 16};
    Sprite s1; s1.layer = -1; s1.uv = rect2{0.5f, 0, 0.5f, 1};
    Handle a = w.create(transform2_at(point2{0, 0}), s0);
    Handle b = w.create(transform2_at(point2{5, 5}, 1.0f), s1);
    Handle d = w.create(transform2_at(point2{9, 9}), s0);
    w.destroy(b); // the renderer never heard of b: no Remove for it

    // First extract: a Tick, then a Set for every live entity the renderer has not seen.
    Array<SpriteDelta> out;
    w.begin_tick();
    extract_sprites(w, out);
    KV_CHECK(c, out.len == 3 && out[0].kind == SpriteDeltaKind::Tick);
    KV_CHECK(c, out[1].kind == SpriteDeltaKind::Set && out[1].entity == a);
    KV_CHECK(c, out[2].kind == SpriteDeltaKind::Set && out[2].entity == d);
    KV_CHECK(c, out[1].state.layer == 1 && out[1].state.tint == color_red() && out[1].state.size == vec2{32, 16});
    KV_CHECK(c, out[1].state.placement.position == point2{0, 0} && out[1].state.material.is_null());
    KV_CHECK(c, count_kind(out, SpriteDeltaKind::Remove) == 0);

    // Nothing changed: a Tick and nothing else.
    out.clear();
    w.begin_tick();
    extract_sprites(w, out);
    KV_CHECK(c, out.len == 1 && out[0].kind == SpriteDeltaKind::Tick);

    // One moved, one changed its sprite: exactly those two, with the new state.
    out.clear();
    w.begin_tick();
    w.transform(a)->position = point2{2, 0};
    w.sprite(d)->tint = color_blue();
    extract_sprites(w, out);
    KV_CHECK(c, out.len == 3 && count_kind(out, SpriteDeltaKind::Set) == 2);
    KV_CHECK(c, out[1].entity == a && out[1].state.placement.position == point2{2, 0});
    KV_CHECK(c, out[2].entity == d && out[2].state.tint == color_blue());

    // Destroying a known entity queues a Remove; a new entity in the reused slot is a
    // Set with its own handle, so the renderer never confuses the two.
    out.clear();
    w.begin_tick();
    w.destroy(a);
    Handle e = w.create(transform2_at(point2{1, 1}), s1);
    KV_CHECK(c, e.index() == b.index() && e != b); // FIFO: b's slot came back first
    extract_sprites(w, out);
    KV_CHECK(c, out.len == 3);
    KV_CHECK(c, out[1].kind == SpriteDeltaKind::Remove && out[1].entity == a);
    KV_CHECK(c, out[2].kind == SpriteDeltaKind::Set && out[2].entity == e && out[2].state.layer == -1);
    KV_CHECK(c, w.removed.len == 0);

    // Deltas accumulate across ticks until the caller clears: two ticks, two markers.
    out.clear();
    w.begin_tick(); w.transform(e)->position = point2{2, 2}; extract_sprites(w, out);
    w.begin_tick(); w.transform(e)->position = point2{3, 3}; extract_sprites(w, out);
    KV_CHECK(c, out.len == 4 && count_kind(out, SpriteDeltaKind::Tick) == 2 && out[3].state.placement.position == point2{3, 3});

    out.free();
    w.free();
}
