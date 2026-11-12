// collision.compose: a child with a collider is where its parent is this tick when the
// broadphase looks, so a part on a moving boss that moves into a standing shot is hit on
// that tick, not the next.
#include "korvid/collision/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/scene/module.h"
#include "korvid/sprite/module.h"
#include "korvid/sprite/transform2.h"
#include <string.h>

namespace {

using namespace kv;

constinit const KindDesc k_kinds[] = {
    {.name = ".boss", .components = "Transform2", .capacity = 4},
    {.name = ".part", .components = "Transform2, Local2, Parent, Collider2", .capacity = 4},
    {.name = ".turret", .components = "Transform2, Local2, Parent, Collider2", .capacity = 4}, // on a part
    {.name = ".shot", .components = "Transform2, Collider2", .capacity = 4},
};

struct Seen { u32 begins; Handle last_a, last_b; };
constinit Seen g = {};
void sys_watch(TickContext& ctx) {
    for (const OverlapBegin& o : ctx.received<OverlapBegin>()) { ++g.begins; g.last_a = o.a; g.last_b = o.b; }
}
// The boss moves right 10 units a tick, in Sim, as a game's mover does.
void sys_move(TickContext& ctx) {
    Table* t = ctx.write_kind("k.boss");
    for (Transform2& x : t->rows_of<Transform2>()) x.position.x += 10.0f;
}
constinit const SystemDesc k_systems[] = {
    {.name = ".move", .phase = Phase::Sim, .fn = sys_move, .writes = ".boss"},
    {.name = ".watch", .phase = Phase::PostSim, .fn = sys_watch, .reads = "OverlapBegin"},
};
KV_MODULE_DEFINE(k_k) = {.name = "k", .kinds = k_kinds, .systems = k_systems};

} // namespace

void test_compose(kv::TestCtx& c) {
    using namespace kv;
    c.begin("compose");

    const ModuleDesc* const list[] = {&sprite_module, &scene_module, &collision_module, &k_k};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    Table* boss = engine_table(e, "k.boss");
    Table* part = engine_table(e, "k.part");
    Table* turret = engine_table(e, "k.turret");
    Table* shot = engine_table(e, "k.shot");
    auto tick = [&](u64 t) { e.phases.run_tick(e, e.events, 1.0 / 60.0, t); };

    // The boss at x 0; its part 20 to the right of it, a turret 10 right of the part. A shot
    // stands at x 50: the turret (radius 4) reaches it once the boss is at 20 or past.
    Handle b = boss->spawn(transform2_at(point2{0, 0}));
    Handle p = part->spawn(transform2_at(point2{20, 0}), Local2{point2{20, 0}}, Parent{b}, collider_circle(4, 1, 2));
    Handle t = turret->spawn(transform2_at(point2{30, 0}), Local2{point2{10, 0}}, Parent{p}, collider_circle(4, 1, 2));
    Handle s = shot->spawn(transform2_at(point2{50, 0}), collider_circle(4, 2, 1));
    g = Seen{};
    tick(0); // boss at 10, turret at 40: 10 apart, no touch
    KV_CHECK(c, g.begins == 0 && turret->get<Transform2>(t)->position.x == 40.0f);
    tick(1); // boss at 20, turret at 50: on the shot this tick
    KV_CHECK(c, g.begins == 1 && ((g.last_a == t && g.last_b == s) || (g.last_a == s && g.last_b == t)));
    KV_CHECK(c, part->get<Transform2>(p)->position.x == 40.0f);

    // The schedule says so: the compose after the mover, the broadphase after the compose.
    i32 at_move = -1, at_compose = -1, at_broad = -1, i = 0;
    for (const ScheduledSystem& sys : e.phases.systems[(u32)Phase::Sim]) {
        if (!strcmp(sys.name(), "k.move")) at_move = i;
        if (!strcmp(sys.name(), "collision.compose")) at_compose = i;
        if (!strcmp(sys.name(), "collision.broadphase")) at_broad = i;
        ++i;
    }
    KV_CHECK(c, at_move >= 0 && at_move < at_compose && at_compose < at_broad);
    engine_shutdown(e);
}
