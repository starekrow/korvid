// The hierarchy: a child follows its parent through position, rotation and scale; a
// grandchild in a kind declared before its parent's kind still composes after it; the
// previous copy makes interpolation right; orphans are destroyed or detached by policy;
// a cycle is skipped and logged; a paused tick composes too.
#include "korvid/scene/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/sprite/module.h"
#include "korvid/sprite/transform2.h"
#include "korvid/sprite/world.h"
#include "korvid/math/scalar.h"

namespace {

using namespace kv;

// Grandchildren first on purpose: table order must not be composition order.
constinit const KindDesc k_kinds[] = {
    {.name = ".grandchild", .components = "Transform2, Sprite, Local2, Parent", .capacity = 64},
    {.name = ".child",      .components = "Transform2, Sprite, Local2, Parent", .capacity = 64},
};
KV_MODULE_DEFINE(k_s) = {.name = "s", .kinds = k_kinds};

bool near(point2 a, point2 b, f32 eps = 1e-3f) { return kv::fabs(a.x - b.x) < eps && kv::fabs(a.y - b.y) < eps; }
bool near(f32 a, f32 b, f32 eps = 1e-4f) { return kv::fabs(a - b) < eps; }

} // namespace

void test_scene(kv::TestCtx& c) {
    using namespace kv;
    c.begin("scene");

    const ModuleDesc* const list[] = {&sprite_module, &scene_module, &k_s};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    SpriteWorld& roots = *sprite_world(e);
    Table* children    = engine_table(e, "s.child");
    Table* grand       = engine_table(e, "s.grandchild");
    u64 tick = 0;
    auto run = [&](bool paused = false) { e.phases.run_tick(e, e.events, 1.0 / 60.0, paused ? tick : tick++, paused); };

    // Placement math on its own.
    Placement2 p = transform2_at(point2{100, 100}, KV_PI * 0.5f, vec2{2, 2});
    Placement2 w = placement_compose(p, local2_at(point2{10, 0}, 0.1f, vec2{0.5f, 0.5f}));
    KV_CHECK(c, near(w.position, point2{100, 120}) && near(w.rotation, KV_PI * 0.5f + 0.1f) && near(w.scale.x, 1.0f));

    // A child 10 to the right of a root at (100, 100): world (110, 100) after a tick.
    Handle root  = roots.create(transform2_at(point2{100, 100}), Sprite{});
    Handle child = children->spawn(Transform2{}, Sprite{}, local2_at(point2{10, 0}), Parent{root});
    run();
    const Transform2* ct = children->get<Transform2>(child);
    KV_CHECK(c, near(ct->position, point2{110, 100}) && near(ct->rotation, 0.0f) && near(ct->scale.x, 1.0f));
    KV_CHECK(c, scene_stats(e)->composed == 1 && scene_stats(e)->max_depth == 1);

    // Rotate the root a quarter turn and double it: the child is now above it, 20 away,
    // and its previous copy is where it was, so the renderer interpolates the swing.
    *roots.transform(root) = transform2_at(point2{100, 100}, KV_PI * 0.5f, vec2{2, 2});
    run();
    KV_CHECK(c, near(ct->position, point2{100, 120}) && near(ct->rotation, KV_PI * 0.5f) && near(ct->scale.y, 2.0f));
    KV_CHECK(c, near(children->prev<Transform2>(child)->position, point2{110, 100}));

    // A grandchild in the kind declared first: composed after its parent all the same.
    Handle gc = grand->spawn(Transform2{}, Sprite{}, local2_at(point2{5, 0}), Parent{child});
    run();
    KV_CHECK(c, near(grand->get<Transform2>(gc)->position, point2{100, 130})); // child's frame is rotated and scaled
    KV_CHECK(c, scene_stats(e)->composed == 2 && scene_stats(e)->max_depth == 2);

    // Attach and detach through the helpers.
    Handle other = roots.create(transform2_at(point2{0, 0}), Sprite{});
    KV_CHECK(c, scene_attach(*grand, gc, other, local2_at(point2{1, 2})));
    run();
    KV_CHECK(c, near(grand->get<Transform2>(gc)->position, point2{1, 2}));
    KV_CHECK(c, scene_detach(*grand, gc));
    roots.transform(other)->position = point2{50, 50};
    run();
    KV_CHECK(c, near(grand->get<Transform2>(gc)->position, point2{1, 2})); // stays where it was
    KV_CHECK(c, !scene_attach(*roots.table, root, other, Local2{}));        // roots have no Parent column

    // Orphans. The child's policy is Destroy: when the root dies the child is destroyed
    // at the end of the tick. A Detach child stays, as a root, where it last was.
    Handle detach_root  = roots.create(transform2_at(point2{7, 7}), Sprite{});
    Handle detach_child = children->spawn(Transform2{}, Sprite{}, local2_at(point2{1, 1}), Parent{detach_root, OrphanPolicy::Detach});
    run();
    KV_CHECK(c, near(children->get<Transform2>(detach_child)->position, point2{8, 8}));
    roots.destroy(root);
    roots.destroy(detach_root);
    run();
    KV_CHECK(c, !children->valid(child));
    KV_CHECK(c, children->valid(detach_child) && children->get<Parent>(detach_child)->parent.is_null());
    KV_CHECK(c, near(children->get<Transform2>(detach_child)->position, point2{8, 8}));
    KV_CHECK(c, scene_stats(e)->orphaned == 2);

    // A cycle: two children pointing at each other are left alone and counted.
    Handle a = children->spawn(transform2_at(point2{1, 0}), Sprite{}, local2_at(point2{1, 0}), Parent{});
    Handle b = children->spawn(transform2_at(point2{2, 0}), Sprite{}, local2_at(point2{1, 0}), Parent{a});
    children->get<Parent>(a)->parent = b;
    run();
    KV_CHECK(c, scene_stats(e)->cycles == 2 && near(children->get<Transform2>(a)->position, point2{1, 0}));
    children->destroy(a);
    children->destroy(b);

    // A paused tick composes too: a menu's children follow their parent while the sim holds.
    Handle menu = roots.create(transform2_at(point2{0, 0}), Sprite{});
    Handle item = children->spawn(Transform2{}, Sprite{}, local2_at(point2{0, -20}), Parent{menu});
    run(true);
    KV_CHECK(c, near(children->get<Transform2>(item)->position, point2{0, -20}));
    roots.transform(menu)->position = point2{3, 3};
    run(true);
    KV_CHECK(c, near(children->get<Transform2>(item)->position, point2{3, -17}));

    engine_shutdown(e);
}
