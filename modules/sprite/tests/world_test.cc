#include "korvid/sprite/world.h"
#include "korvid/core/test.h"
#include <string.h>

void test_sprite_world(kv::TestCtx& c) {
    using namespace kv;
    c.begin("sprite_world");

    SpriteWorld w;
    Sprite s;
    s.layer = 3;
    Handle a = w.create(transform2_at(point2{1, 2}), s);
    Handle b = w.create(transform2_at(point2{3, 4}), s);
    KV_CHECK(c, w.live() == 2);
    KV_CHECK(c, w.transform(a) && w.transform(a)->position == point2{1, 2});
    KV_CHECK(c, w.sprite(b) && w.sprite(b)->layer == 3);

    // A new entity starts with prev == cur.
    KV_CHECK(c, w.prev_transforms[a.index()].position == point2{1, 2});

    // begin_tick copies cur into prev; moving afterwards leaves prev behind.
    w.transform(a)->position = point2{10, 2};
    w.begin_tick();
    w.transform(a)->position = point2{20, 2};
    KV_CHECK(c, w.prev_transforms[a.index()].position == point2{10, 2});
    KV_CHECK(c, w.transform(a)->position == point2{20, 2});

    // Destroy, then use the stale handle: null, never another entity's data.
    KV_CHECK(c, w.destroy(a));
    KV_CHECK(c, w.live() == 1);
    KV_CHECK(c, w.transform(a) == nullptr);
    KV_CHECK(c, w.sprite(a) == nullptr);
    KV_CHECK(c, !w.destroy(a));

    // The slot is reused with a new generation and fresh history.
    Handle d = w.create(transform2_at(point2{7, 7}), s);
    KV_CHECK(c, d.index() == a.index() && d.gen() != a.gen());
    KV_CHECK(c, w.transform(a) == nullptr);
    KV_CHECK(c, w.transform(d)->position == point2{7, 7});
    KV_CHECK(c, w.prev_transforms[d.index()].position == point2{7, 7});

    w.free();
    KV_CHECK(c, w.live() == 0);
    KV_CHECK(c, w.transforms.len == 0);

    // With init, the world is a handle issuer: a blind resolve of an entity is its
    // transform, the type key says so, and the description is readable.
    SpriteWorld world;
    KV_CHECK(c, world.init(nullptr, 1024, MemTag{}) && world.kind != 0);
    Sprite sp; sp.layer = 4;
    Handle e = world.create(transform2_at(point2{3, 4}), sp);
    KV_CHECK(c, e.kind() == world.kind);
    KV_CHECK(c, resolve(e) == world.transform(e));
    KV_CHECK(c, resolve<Transform2>(e) == world.transform(e) && resolve<Sprite>(e) == nullptr);
    char text[128];
    kv_handle_describe(e.bits, text, sizeof text);
    KV_CHECK(c, strcmp(text, "sprite.entity#0:1 at (3.0, 4.0) layer 4 (1 live)") == 0);
    world.destroy(e);
    kv_handle_describe(e.bits, text, sizeof text);
    KV_CHECK(c, strstr(text, "stale") != nullptr && resolve(e) == nullptr);
    u16 kind = world.kind;
    world.free();
    KV_CHECK(c, !kv_handle_kind_is_live(kind));
}
