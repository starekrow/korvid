// example_arcfire: the ship. Eight directions at two speeds, the focus that slows it,
// shows its hitbox and narrows the shot, and Spread at its first level: two shots every
// six ticks while the button is held. The hitbox is the ship's own collider, a circle of
// three units; the graze ring around it is a child row (a row has one collider).
#include "arcfire.h"
#include "korvid/platform_hid/actions.h"
#include <math.h>

namespace kv::arcfire {

namespace {

constexpr f32 kSpeed = 6.0f, kFocusSpeed = 3.0f; // units a tick
constexpr f32 kHalf = 32.0f;                      // the sprite's half size: the clamp
constexpr u32 kCadence = 6;                       // ticks between volleys
constexpr f32 kShotSpeed = 14.0f;
constexpr f32 kSpreadDeg = 6.0f, kFocusSpreadDeg = 2.0f;
constexpr f32 kHitbox = 3.0f, kGrazeRing = 18.0f;

} // namespace

Collider2 ship_collider() { return collider_circle(kHitbox, L_SHIP, L_BULLET | L_AIR); }
Collider2 ring_collider() { return collider_circle(kGrazeRing, L_GRAZE, L_BULLET); }

Handle ship_spawn(Engine& e, Game& g) {
    Table* ships = engine_table(e, g_ship_kind);
    Handle ship = ships->spawn(transform2_at(kShipStart), sprite_of(g, TEX_SHIPS, ship_uv(0, 0), vec2{64, 64}, Z_SHIP), ship_collider(),
                               Ship{0, 1, 0, 0});
    // The ring and the dot show only while focused.
    Sprite ring = sprite_of(g, TEX_RING, rect2_unit(), vec2{2 * kGrazeRing, 2 * kGrazeRing}, Z_MARK, color{0.6f, 0.9f, 1.0f, 0.0f}, true);
    g.ring = engine_table(e, g_ring_kind)->spawn(transform2_at(kShipStart), Local2{}, Parent{ship}, ring, ring_collider());
    Sprite dot = sprite_of(g, TEX_DOT, rect2_unit(), vec2{8, 8}, Z_MARK + 1, color{1.0f, 0.3f, 0.4f, 0.0f});
    g.mark = engine_table(e, g_mark_kind)->spawn(transform2_at(kShipStart), Local2{}, Parent{ship}, dot);
    engine_entity_name_set(e, ship, "ship");
    return ship;
}

// Phase Sim: the actions fly the ship and fire. While the egg plays the ship is not live
// and nothing here moves it.
void control(TickContext& ctx) {
    static const u32 k_actions_id = ctx.resource_id("input.actions");
    Game& g = ctx.state_as<Game>();
    const InputActions& a = *ctx.resource<const InputActions>(k_actions_id);
    Span<Transform2> xf = ctx.write<Transform2>("example_arcfire.ship.Transform2");
    Span<Ship> ships = ctx.write<Ship>("example_arcfire.ship.Ship");
    Span<Sprite> ring = ctx.write<Sprite>("example_arcfire.ring.Sprite");
    Span<Sprite> mark = ctx.write<Sprite>("example_arcfire.mark.Sprite");
    u32 i = g.ship.index();
    if (g.ship.is_null() || i >= ships.len) return;
    Ship& s = ships[i];
    bool focus = s.live && action_down(a, g.act_focus);
    s.focus = focus;
    if (g.ring.index() < ring.len) ring[g.ring.index()].tint.a = focus ? 0.55f : 0.0f;
    if (g.mark.index() < mark.len) mark[g.mark.index()].tint.a = focus ? 1.0f : 0.0f;
    if (s.cooldown) --s.cooldown;
    if (!s.live) return;

    vec2 dir = action_axis(a, g.act_move);
    point2& p = xf[i].position;
    p += dir * (focus ? kFocusSpeed : kSpeed);
    p.x = clampf(p.x, kFieldLeft + kHalf, kFieldRight - kHalf);
    p.y = clampf(p.y, kFieldBottom + kHalf, kFieldTop - kHalf);

    if (!action_down(a, g.act_shot) || s.cooldown) return;
    s.cooldown = kCadence;
    Table* shots_table = ctx.write_kind(g_shot_kind);
    f32 spread = (focus ? kFocusSpreadDeg : kSpreadDeg) * (KV_PI / 180.0f);
    for (i32 side = -1; side <= 1; side += 2) {
        f32 ang = (f32)side * spread; // from straight up, clockwise for +
        vec2 v{sinf(ang) * kShotSpeed, cosf(ang) * kShotSpeed};
        Sprite sp = sprite_of(g, TEX_TILES, tile_uv(0, 0), vec2{24, 24}, Z_SHOT);
        shots_table->spawn(transform2_at(p + vec2{(f32)side * 8.0f, 18.0f}, -ang), sp, Velocity{v}, collider_circle(6.0f, L_SHOT, L_AIR | L_GROUND),
                           Shot{1, 0});
        ++s.shots;
    }
}

// Phase Sim, after control: shots fly; what leaves the field goes.
void shots(TickContext& ctx) {
    for (KindRows k : ctx.view<Transform2, const Velocity, const Shot>()) {
        Span<Transform2> t = k.col<Transform2>();
        Span<const Velocity> v = k.col<const Velocity>();
        parallel_for(ctx, k.rows(), 512, [&](ChunkContext& cc, u32 begin, u32 end) {
            for (u32 i = begin; i < end; ++i) {
                if (!k.alive(i)) continue;
                t[i].position += v[i].v;
                if (!in_field(t[i].position, kCullMargin)) cc.destroy(k.handle_at(i));
            }
        });
    }
}

} // namespace kv::arcfire
