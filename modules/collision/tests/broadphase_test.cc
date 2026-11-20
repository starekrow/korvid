// The broadphase: overlapping circles begin, separated ones end, masks filter, boxes and
// circles mix, pairs are canonical, a destroyed entity's End is dropped as stale, and
// many colliders across cells find exactly the pairs that touch.
#include "korvid/collision/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/sprite/module.h"
#include "korvid/scene/module.h"
#include "korvid/sprite/transform2.h"

namespace {

using namespace kv;

constinit const KTableDesc k_ktables[] = {{.name = ".thing", .components = "Transform2, Collider2", .capacity = 4096}};

struct Seen { u32 begins, ends; Handle last_a, last_b; };
constinit Seen g = {};
void sys_watch(TickContext& ctx) {
    for (const OverlapBegin& o : ctx.received<OverlapBegin>()) { ++g.begins; g.last_a = o.a; g.last_b = o.b; }
    g.ends += ctx.received<OverlapEnd>().len;
}
constinit const SystemDesc k_systems[] = {{.name = ".watch", .phase = Phase::PostSim, .fn = sys_watch, .reads = "OverlapBegin, OverlapEnd"}};
KV_MODULE_DEFINE(k_c) = {.name = "c", .ktables = k_ktables, .systems = k_systems};

} // namespace

void test_broadphase(kv::TestCtx& c) {
    using namespace kv;
    c.begin("broadphase");

    const ModuleDesc* const list[] = {&sprite_module, &scene_module, &collision_module, &k_c};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KTable* things = engine_ktable(e, "c.thing");
    auto tick = [&](u64 t) { e.phases.run_tick(e, e.events, 1.0 / 60.0, t); };

    // Two circles 30 apart with radii 20 and 15: overlapping. Begin once, then nothing.
    Handle a = things->spawn(transform2_at(point2{0, 0}), collider_circle(20, 1, 0xFF));
    Handle b = things->spawn(transform2_at(point2{30, 0}), collider_circle(15, 1, 0xFF));
    g = Seen{};
    tick(0);
    KV_CHECK(c, g.begins == 1 && g.ends == 0 && collision_pair_count(e) == 1);
    KV_CHECK(c, g.last_a.bits < g.last_b.bits && (g.last_a == a || g.last_a == b)); // canonical
    tick(1);
    KV_CHECK(c, g.begins == 1 && g.ends == 0);

    // Move b away: End, once.
    things->get<Transform2>(b)->position = point2{100, 0};
    tick(2);
    KV_CHECK(c, g.begins == 1 && g.ends == 1 && collision_pair_count(e) == 0);

    // Masks: c is on layer 2 and a accepts only layer 1 bits... a's mask is 0xFF, so a
    // accepts c; but c's mask excludes a's layer: no pair. Both directions must agree.
    Handle cc = things->spawn(transform2_at(point2{10, 0}), collider_circle(10, 2, 0x2));
    tick(3);
    KV_CHECK(c, g.begins == 1);
    things->get<Collider2>(cc)->mask = 0x1;
    tick(4);
    KV_CHECK(c, g.begins == 2);

    // A box and a circle: the circle's center is outside the box but its edge reaches in.
    Handle box = things->spawn(transform2_at(point2{500, 500}), collider_box(vec2{20, 10}, 1, 0xFF));
    Handle cir = things->spawn(transform2_at(point2{535, 500}), collider_circle(16, 1, 0xFF));
    tick(5);
    KV_CHECK(c, g.begins == 3);
    things->get<Transform2>(cir)->position = point2{537, 500}; // 37 > 20 + 16: apart
    tick(6);
    KV_CHECK(c, g.ends == 2);
    KV_UNUSED(box);

    // Destroying one of an overlapping pair: the End names a dead entity and is dropped.
    u64 dropped = e.events.dropped_stale;
    things->destroy(cc);
    tick(7);
    KV_CHECK(c, g.ends == 2 && e.events.dropped_stale == dropped + 1 && collision_pair_count(e) == 0);

    // A field of 400 small circles on a 20-unit lattice with radius 11: each touches its
    // four orthogonal neighbors (distance 20 < 22) and not its diagonals (28.3 > 22).
    // 20x20 lattice: 2 * 20 * 19 = 760 pairs, far from the others.
    g = Seen{};
    for (u32 y = 0; y < 20; ++y)
        for (u32 x = 0; x < 20; ++x)
            things->spawn(transform2_at(point2{5000.0f + 20.0f * x, 5000.0f + 20.0f * y}), collider_circle(11, 4, 4));
    tick(8);
    KV_CHECK(c, g.begins == 760 && g.ends == 0);
    tick(9);
    KV_CHECK(c, g.begins == 760 && g.ends == 0);

    engine_shutdown(e);
}
