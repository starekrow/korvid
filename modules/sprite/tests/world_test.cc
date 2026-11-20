// The sprite world is the engine's "sprite.entity" ktable: create and typed lookups,
// previous transforms kept by the engine, stale handles, slot reuse, and the entity as a
// handle the registry can resolve and describe.
#include "korvid/sprite/world.h"
#include "korvid/sprite/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include <string.h>

void test_sprite_world(kv::TestCtx& c) {
    using namespace kv;
    c.begin("sprite_world");

    const ModuleDesc* const list[] = {&sprite_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    SpriteWorld& w = *sprite_world(e);
    KV_CHECK(c, w.ktable && w.kind() != 0 && engine_ktable(e, "sprite.entity") == w.ktable);
    KV_CHECK(c, engine_resource_id(e, "sprite.entity") && engine_resource_id(e, "sprite.entity.Transform2") &&
                engine_resource_id(e, "sprite.entity.Sprite"));

    Sprite s;
    s.layer = 3;
    Handle a = w.create(transform2_at(point2{1, 2}), s);
    Handle b = w.create(transform2_at(point2{3, 4}), s);
    KV_CHECK(c, w.live() == 2 && a.kind() == w.kind());
    KV_CHECK(c, w.transform(a) && w.transform(a)->position == point2{1, 2});
    KV_CHECK(c, w.sprite(b) && w.sprite(b)->layer == 3);

    // A new entity starts with prev == cur.
    KV_CHECK(c, w.prev_transform(a)->position == point2{1, 2});

    // The engine copies cur into prev at the top of PreSim; moving afterwards leaves prev behind.
    w.transform(a)->position = point2{10, 2};
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 0);
    w.transform(a)->position = point2{20, 2};
    KV_CHECK(c, w.prev_transform(a)->position == point2{10, 2});
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
    KV_CHECK(c, w.prev_transform(d)->position == point2{7, 7});

    // A blind resolve of an entity is its transform (the ktable's first component), the
    // type key says so, and the description is every component through its schema.
    Sprite sp; sp.layer = 4;
    Handle f = w.create(transform2_at(point2{3, 4}), sp);
    KV_CHECK(c, resolve(f) == w.transform(f));
    KV_CHECK(c, resolve<Transform2>(f) == w.transform(f) && resolve<Sprite>(f) == nullptr);
    char text[256];
    kv_handle_describe(f.bits, text, sizeof text);
    KV_CHECK(c, strcmp(text, "sprite.entity#2:1 Transform2{position=(3, 4), rotation=0, scale=(1, 1)} "
                             "Sprite{material=null, uv=(0, 0, 1, 1), size=(64, 64), tint=(1, 1, 1, 1), layer=4, space=0, anchor=0, sort=0, order=0}") == 0);
    w.destroy(f);
    kv_handle_describe(f.bits, text, sizeof text);
    KV_CHECK(c, strstr(text, "stale") != nullptr && resolve(f) == nullptr);

    u16 kind = w.kind();
    engine_shutdown(e);
    KV_CHECK(c, !kv_handle_kind_is_live(kind));
}
