// The resolve's fence posts: touching is not overlapping; a stopped box ends exactly on the
// edge it met; a box slides along a row of boxes without stopping at the seams; nothing
// tunnels; a box inside another may only leave it; two movers resolve in kind and slot
// order; a push moves the pushed body by what the pusher gains, at its pace, and stops
// against what is behind it; the static world stops a box like a wall; a Sim system that
// reads MoveResult2 sees this tick's result; a child moves its Local2; intents last a
// tick; a sensor's overlap arrives the tick of the move; queries and casts.
#include "korvid/collision/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/scene/module.h"
#include "korvid/scene/parent.h"
#include "korvid/sprite/module.h"
#include "korvid/sprite/transform2.h"
#include "korvid/sprite/world.h"

namespace {

using namespace kv;

constinit const KindDesc k_kinds[] = {
    {.name = ".walker", .components = "Transform2, Collider2, Move2, MoveResult2", .capacity = 64},
    {.name = ".wall",   .components = "Transform2, Collider2", .capacity = 256},
    {.name = ".crate",  .components = "Transform2, Collider2, Move2, MoveResult2", .capacity = 16},
    {.name = ".kid",    .components = "Transform2, Local2, Parent, Collider2, Move2", .capacity = 4},
    {.name = ".zone",   .components = "Transform2, Collider2", .capacity = 4},
};

struct Seen { u32 hits_x; u32 begins; u32 moved_others; Handle blocker; u32 queried; CollisionHit cast; };
constinit Seen g = {};

// Sim, after the resolve because it reads what the resolve writes.
void sys_react(TickContext& ctx) {
    for (KindRows k : ctx.view<const MoveResult2>()) {
        Span<const MoveResult2> r = k.col<const MoveResult2>();
        for (u32 i = 0; i < k.rows(); ++i)
            if (k.alive(i) && r[i].hit_x) { ++g.hits_x; g.blocker = r[i].blocker; }
    }
    g.moved_others = ctx.resource<const Array<Handle>>("collision.moved")->len;
}
void on_begin(TickContext&, Span<const OverlapBegin> b) { g.begins += b.len; }
// PostSim: the queries see this tick's positions.
void sys_query(TickContext& ctx) {
    MemScope scope;
    Array<Handle> out = array_in<Handle>(mem_scratch());
    g.queried = collision_query(ctx, rect2{-1000, -1000, 2000, 2000}, 0x4, out);
    g.cast = collision_cast(ctx, rect2{-200, -2, 4, 4}, vec2{400, 0}, 0x2);
}

constinit const SystemDesc k_systems[] = {
    {.name = ".react", .phase = Phase::Sim, .fn = sys_react, .reads = "*.MoveResult2, collision.moved"},
    {.name = ".query", .phase = Phase::PostSim, .fn = sys_query, .reads = "*[Collider2].Transform2, *[Collider2].Collider2"},
};
constinit const HandlerDesc k_handlers[] = {{.name = ".on_begin", KV_ON(OverlapBegin, on_begin), .phase = Phase::PostSim}};
KV_MODULE_DEFINE(k_r) = {.name = "r", .kinds = k_kinds, .systems = k_systems, .handlers = k_handlers};

constexpr u32 L_BODY = 0x2, L_ZONE = 0x4;

Collider2 body(BodyKind k, f32 hx, f32 hy, u8 flags = 0) { return collider_body(k, vec2{hx, hy}, vec2{0, 0}, L_BODY, L_BODY | L_ZONE, flags); }

// A wall at x = 50 and beyond, for the static world.
rect2 static_wall(void*, rect2 box, vec2 d, bool* hit, bool*) {
    if (d.x > 0.0f && box.x + box.w + d.x > 50.0f) { *hit = true; box.x = 50.0f - box.w; return box; }
    box.x += d.x; box.y += d.y;
    return box;
}

} // namespace

void test_resolve(kv::TestCtx& c) {
    using namespace kv;
    c.begin("resolve");
    const ModuleDesc* const list[] = {&sprite_module, &scene_module, &collision_module, &k_r};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    Table* walkers = engine_table(e, "r.walker");
    Table* walls = engine_table(e, "r.wall");
    Table* crates = engine_table(e, "r.crate");
    u64 tick = 0;
    auto run = [&]() { e.phases.run_tick(e, e.events, 1.0 / 60.0, tick++); };
    auto go = [&](Table* t, Handle h, f32 dx, f32 dy) { t->get<Move2>(h)->delta = vec2{dx, dy}; };
    auto x_of = [&](Table* t, Handle h) { return t->get<Transform2>(h)->position.x; };
    auto clear = [&](Table* t) { t->destroy_all(); };

    // ---- touching is not overlapping; exact stops -----------------------------------------
    Handle w = walkers->spawn(transform2_at(point2{0, 0}), body(BodyKind::Kinematic, 5, 5), Move2{}, MoveResult2{});
    Handle wall = walls->spawn(transform2_at(point2{10, 0}), body(BodyKind::Solid, 5, 5)); // [5, 15): touching
    g = Seen{};
    go(walkers, w, 3, 0); run();
    const MoveResult2* r = walkers->get<MoveResult2>(w);
    KV_CHECK(c, x_of(walkers, w) == 0 && r->hit_x && r->blocker == wall && r->asked.x == 3 && r->moved.x == 0);
    KV_CHECK(c, g.hits_x == 1 && g.blocker == wall); // a Sim reader saw it this tick
    KV_CHECK(c, walkers->get<Move2>(w)->delta.x == 0); // the intent lasted a tick
    go(walkers, w, -3, 0); run();
    KV_CHECK(c, x_of(walkers, w) == -3 && !walkers->get<MoveResult2>(w)->hit_x);
    go(walkers, w, 0, 4); run(); // along the wall: free
    KV_CHECK(c, walkers->get<Transform2>(w)->position.y == 4);
    walls->get<Transform2>(wall)->position = point2{20, 4}; // [15, 25)
    go(walkers, w, 7, 0); run();
    KV_CHECK(c, x_of(walkers, w) == 4);
    go(walkers, w, 7, 0); run();
    KV_CHECK(c, x_of(walkers, w) == 10 && walkers->get<MoveResult2>(w)->hit_x); // its right edge on 15 exactly
    for (u32 i = 0; i < 100; ++i) { go(walkers, w, 1, 0); run(); }
    KV_CHECK(c, x_of(walkers, w) == 10); // no drift into the wall
    clear(walkers); clear(walls); engine_apply_commands(e);

    // ---- a row of boxes: no snag at the seams, either way, either axis order --------------
    for (i32 i = -10; i <= 10; ++i) walls->spawn(transform2_at(point2{(f32)i * 10.0f, 10}), body(BodyKind::Solid, 5, 5)); // y [5, 15)
    w = walkers->spawn(transform2_at(point2{0, 0}), body(BodyKind::Kinematic, 5, 5), Move2{}, MoveResult2{});    // y [-5, 5)
    for (u32 i = 0; i < 40; ++i) { go(walkers, w, 1.25f, 0.5f); run(); }
    KV_CHECK(c, x_of(walkers, w) == 50.0f && walkers->get<Transform2>(w)->position.y == 0 && walkers->get<MoveResult2>(w)->hit_y);
    for (u32 i = 0; i < 80; ++i) { go(walkers, w, -1.25f, 0.5f); run(); }
    KV_CHECK(c, x_of(walkers, w) == -50.0f);
    clear(walkers); clear(walls); engine_apply_commands(e);

    // ---- nothing tunnels ---------------------------------------------------------------------
    w = walkers->spawn(transform2_at(point2{0, 0}), body(BodyKind::Kinematic, 5, 5), Move2{}, MoveResult2{});
    wall = walls->spawn(transform2_at(point2{100, 0}), body(BodyKind::Solid, 1, 50)); // [99, 101)
    go(walkers, w, 1000, 0); run();
    KV_CHECK(c, x_of(walkers, w) == 94);
    go(walkers, w, 0, 0); walkers->get<Transform2>(w)->position.x = 200; run();
    go(walkers, w, -5000, 0); run();
    KV_CHECK(c, x_of(walkers, w) == 106);

    // ---- inside another: out, never deeper ----------------------------------------------------
    walkers->get<Transform2>(w)->position.x = 97; // [92, 102) over [99, 101)
    go(walkers, w, 3, 0); run();
    KV_CHECK(c, x_of(walkers, w) == 97 && walkers->get<MoveResult2>(w)->hit_x);
    go(walkers, w, -3, 0); run();
    KV_CHECK(c, x_of(walkers, w) == 94);
    clear(walkers); clear(walls); engine_apply_commands(e);

    // ---- two movers: kind and slot order; the second sees where the first ended -------------
    Handle a = walkers->spawn(transform2_at(point2{0, 0}), body(BodyKind::Kinematic, 5, 5), Move2{}, MoveResult2{});
    Handle b = walkers->spawn(transform2_at(point2{20, 0}), body(BodyKind::Kinematic, 5, 5), Move2{}, MoveResult2{});
    go(walkers, a, 10, 0); go(walkers, b, -10, 0); run();
    KV_CHECK(c, x_of(walkers, a) == 10 && x_of(walkers, b) == 20); // a went first and took the gap
    KV_CHECK(c, walkers->get<MoveResult2>(b)->blocker == a && walkers->get<MoveResult2>(a)->blocker.is_null());
    clear(walkers); engine_apply_commands(e);

    // ---- pushing: at the pace, by what the pusher gains; blocked behind -----------------------
    Handle p = walkers->spawn(transform2_at(point2{0, 0}), body(BodyKind::Kinematic, 5, 5, ColliderPusher), Move2{}, MoveResult2{});
    Collider2 crate_c = body(BodyKind::Solid, 5, 5, ColliderPushable);
    crate_c.pace = 0.5f;
    Handle cr = crates->spawn(transform2_at(point2{10, 0}), crate_c, Move2{}, MoveResult2{}); // touching
    g = Seen{};
    go(walkers, p, 4, 0); run();
    KV_CHECK(c, x_of(walkers, p) == 2 && x_of(crates, cr) == 12);
    const MoveResult2* pr = walkers->get<MoveResult2>(p);
    const MoveResult2* cres = crates->get<MoveResult2>(cr);
    KV_CHECK(c, pr->pushed == cr && !pr->hit_x && pr->moved.x == 2 && cres->displaced && cres->pushed_by == p && cres->moved.x == 2);
    KV_CHECK(c, g.moved_others == 1);
    // Not a pusher: it stops.
    walkers->get<Collider2>(p)->flags = 0;
    go(walkers, p, 4, 0); run();
    KV_CHECK(c, x_of(walkers, p) == 2 && x_of(crates, cr) == 12 && walkers->get<MoveResult2>(p)->blocker == cr);
    walkers->get<Collider2>(p)->flags = ColliderPusher;
    // A wall behind the crate: the crate goes to it and the pusher stops at the crate.
    walls->spawn(transform2_at(point2{25, 0}), body(BodyKind::Solid, 5, 5)); // [20, 30)
    for (u32 i = 0; i < 10; ++i) { go(walkers, p, 4, 0); run(); }
    KV_CHECK(c, x_of(crates, cr) == 15 && x_of(walkers, p) == 5 && walkers->get<MoveResult2>(p)->hit_x && walkers->get<MoveResult2>(p)->blocker == cr);
    clear(walkers); clear(walls); clear(crates); engine_apply_commands(e);

    // ---- the static world ---------------------------------------------------------------------
    collision_static(e, static_wall, nullptr);
    w = walkers->spawn(transform2_at(point2{0, 0}), body(BodyKind::Kinematic, 5, 5), Move2{}, MoveResult2{});
    go(walkers, w, 100, 0); run();
    KV_CHECK(c, x_of(walkers, w) == 45 && walkers->get<MoveResult2>(w)->hit_x && walkers->get<MoveResult2>(w)->blocker.is_null());
    collision_static(e, nullptr, nullptr);
    clear(walkers); engine_apply_commands(e);

    // ---- a child moves its Local2 by the same delta -------------------------------------------
    SpriteWorld& roots = *sprite_world(e);
    Handle root = roots.create(transform2_at(point2{100, 0}), Sprite{});
    Table* kids = engine_table(e, "r.kid");
    Handle kid = kids->spawn(transform2_at(point2{100, 0}), local2_at(point2{0, 0}), Parent{root}, body(BodyKind::Kinematic, 2, 2), Move2{});
    go(kids, kid, 5, 0); run();
    KV_CHECK(c, kids->get<Local2>(kid)->position.x == 5 && kids->get<Transform2>(kid)->position.x == 105);
    kids->destroy(kid);

    // ---- a sensor hears the move the tick it happens; queries and casts see it ---------------
    Table* zones = engine_table(e, "r.zone");
    zones->spawn(transform2_at(point2{30, 0}), collider_box(vec2{5, 5}, L_ZONE, L_BODY));
    w = walkers->spawn(transform2_at(point2{0, 0}), body(BodyKind::Kinematic, 5, 5), Move2{}, MoveResult2{});
    walls->spawn(transform2_at(point2{-100, 0}), body(BodyKind::Solid, 5, 5));
    g = Seen{};
    run();
    KV_CHECK(c, g.begins == 0 && g.queried == 1);
    go(walkers, w, 25, 0); run(); // into the zone: the handler runs at this tick's PostSim boundary
    KV_CHECK(c, g.begins == 1);
    KV_CHECK(c, g.cast.entity == walls->handle_at(0) || !g.cast.entity.is_null());
    KV_CHECK(c, g.cast.t > 0.0f && g.cast.t < 1.0f);

    KV_CHECK(c, collision_stats(e) && collision_stats(e)->bodies == 2);
    engine_shutdown(e);
}
