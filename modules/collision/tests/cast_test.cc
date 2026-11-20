// collision_cast_all: every collider a moving box meets on the layers asked, nearest first,
// one it starts inside at 0, ties in ktable and slot order, other layers and misses left out.
#include "korvid/collision/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/memory.h"
#include "korvid/core/test.h"
#include "korvid/scene/module.h"
#include "korvid/sprite/module.h"
#include "korvid/sprite/transform2.h"

namespace {

using namespace kv;

constinit const KTableDesc k_ktables[] = {{.name = ".thing", .components = "Transform2, Collider2", .capacity = 16}};

struct Got { u32 n; CollisionHit hits[8]; };
constinit Got g = {};
void sys_cast(TickContext& ctx) {
    MemScope scope;
    Array<CollisionHit> out = array_in<CollisionHit>(mem_scratch());
    out.push(CollisionHit{}); // something already there: the hits go after it, sorted among themselves
    g.n = collision_cast_all(ctx, rect2{-2, -2, 4, 4}, vec2{100, 0}, 0x1, out);
    for (u32 i = 0; i < g.n && i < 8; ++i) g.hits[i] = out[1 + i];
}
constinit const SystemDesc k_systems[] = {
    {.name = ".cast", .phase = Phase::PostSim, .fn = sys_cast, .reads = "*[Collider2].Transform2, *[Collider2].Collider2"},
};
KV_MODULE_DEFINE(k_x) = {.name = "x", .ktables = k_ktables, .systems = k_systems};

} // namespace

void test_cast_all(kv::TestCtx& c) {
    using namespace kv;
    c.begin("cast_all");

    const ModuleDesc* const list[] = {&sprite_module, &scene_module, &collision_module, &k_x};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KTable* things = engine_ktable(e, "x.thing");
    // Spawned far first, so slot order is not distance order.
    Handle far   = things->spawn(transform2_at(point2{70, 0}), collider_circle(5, 0x1, 0));
    Handle other = things->spawn(transform2_at(point2{40, 0}), collider_circle(5, 0x2, 0)); // another layer
    Handle tie_a = things->spawn(transform2_at(point2{30, 1}), collider_box(vec2{3, 3}, 0x1, 0));
    Handle tie_b = things->spawn(transform2_at(point2{30, -1}), collider_box(vec2{3, 3}, 0x1, 0));
    Handle inside = things->spawn(transform2_at(point2{0, 0}), collider_circle(3, 0x1, 0));
    Handle beyond = things->spawn(transform2_at(point2{140, 0}), collider_circle(5, 0x1, 0)); // past the move
    Handle aside  = things->spawn(transform2_at(point2{50, 30}), collider_circle(5, 0x1, 0)); // off the path
    (void)other; (void)beyond; (void)aside;
    g = Got{};
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 0);
    KV_CHECK(c, g.n == 4);
    KV_CHECK(c, g.hits[0].entity == inside && g.hits[0].t == 0.0f);
    KV_CHECK(c, g.hits[1].entity == tie_a && g.hits[2].entity == tie_b && g.hits[1].t == g.hits[2].t); // a tie: slot order
    // The box's front (x + 2) reaches the tie's back edge (27) a quarter of the way along.
    KV_CHECK(c, g.hits[1].t > 0.24f && g.hits[1].t < 0.26f);
    KV_CHECK(c, g.hits[3].entity == far && g.hits[3].t > g.hits[2].t);
    engine_shutdown(e);
}
