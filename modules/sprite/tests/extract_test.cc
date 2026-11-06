#include "korvid/sprite/extract.h"
#include "korvid/sprite/world.h"
#include "korvid/core/test.h"

void test_extract(kv::TestCtx& c) {
    using namespace kv;
    c.begin("extract");

    SpriteWorld w;
    Sprite s0; s0.layer = 1; s0.tint = color_red(); s0.size = vec2{32, 16};
    Sprite s1; s1.layer = -1; s1.uv = rect2{0.5f, 0, 0.5f, 1};
    Handle a = w.create(transform2_at(point2{0, 0}), s0);
    Handle b = w.create(transform2_at(point2{5, 5}, 1.0f), s1);
    Handle d = w.create(transform2_at(point2{9, 9}), s0);
    w.destroy(b);

    w.begin_tick();
    w.transform(a)->position = point2{2, 0};

    Array<RenderPacket> packets;
    packets.push(RenderPacket{}); // extract must clear stale contents
    extract_sprites(w, packets);

    // One packet per live entity, in slot order, dead slots skipped.
    KV_CHECK(c, packets.len == 2);
    KV_CHECK(c, packets[0].entity == a);
    KV_CHECK(c, packets[1].entity == d);
    KV_CHECK(c, w.valid(packets[0].entity) && w.valid(packets[1].entity));

    // Both transforms, and the sprite fields, come across.
    KV_CHECK(c, packets[0].prev.position == point2{0, 0});
    KV_CHECK(c, packets[0].cur.position == point2{2, 0});
    KV_CHECK(c, packets[0].layer == 1);
    KV_CHECK(c, packets[0].tint == color_red());
    KV_CHECK(c, packets[0].size == vec2{32, 16});
    KV_CHECK(c, packets[0].material.is_null());
    KV_CHECK(c, packets[1].uv.x == 0.0f && packets[1].uv.w == 1.0f);

    // Interpolation at alpha 0.5 lands halfway.
    Placement2 mid = lerp(packets[0].prev, packets[0].cur, 0.5f);
    KV_CHECK(c, mid.position == point2{1, 0});

    // Reusing the dead slot puts the new entity back in slot order.
    Handle e = w.create(transform2_at(point2{1, 1}), s1);
    extract_sprites(w, packets);
    KV_CHECK(c, packets.len == 3);
    KV_CHECK(c, packets[1].entity == e);
    KV_CHECK(c, packets[1].entity != b);

    packets.free();
    w.free();
}
