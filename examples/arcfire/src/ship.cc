// arcfire: the ship. Eight directions at two speeds, the focus that slows it,
// shows its hitbox and narrows the shot, and its weapon (weapons.cc) fired while the button
// is held. The hitbox is the ship's own collider, a circle of three units; the graze ring
// around it, and its beam, are child rows (a row has one collider).
#include "arcfire.h"
#include "korvid/platform_hid/actions.h"

namespace kv::arcfire {

namespace {

constexpr f32 kSpeed = 6.0f, kFocusSpeed = 3.0f; // units a tick
constexpr f32 kHalf = 32.0f;                      // the sprite's half size: the clamp
constexpr f32 kHitbox = 3.0f, kGrazeRing = 18.0f;

} // namespace

Collider2 ship_collider() { return collider_circle(kHitbox, L_SHIP, L_BULLET | L_AIR); }
Collider2 ring_collider() { return collider_circle(kGrazeRing, L_GRAZE, L_BULLET | L_ITEM); }

Handle ship_spawn(Engine& e, Game& g, const Dials& d) {
    KTable* ships = engine_ktable(e, g_ship_ktable);
    Handle ship = ships->spawn(transform2_at(kShipStart), sprite_of(g, TEX_SHIPS, ship_uv(0, 0), vec2{64, 64}, Z_SHIP), ship_collider(),
                               Ship{0, 1, 0, 0, 0, 0, d.weapon < WeaponCount ? d.weapon : 0, d.power < 1 ? 1 : d.power > kMaxPower ? kMaxPower : d.power, 0, 0});
    // The ring and the dot show only while focused.
    Sprite ring = sprite_of(g, TEX_RING, rect2_unit(), vec2{2 * kGrazeRing, 2 * kGrazeRing}, Z_MARK, color{0.6f, 0.9f, 1.0f, 0.0f}, true);
    g.ring = engine_ktable(e, g_ring_ktable)->spawn(transform2_at(kShipStart), Local2{}, Parent{ship}, ring, ring_collider());
    Sprite dot = sprite_of(g, TEX_DOT, rect2_unit(), vec2{8, 8}, Z_MARK + 1, color{1.0f, 0.3f, 0.4f, 0.0f});
    g.mark = engine_ktable(e, g_mark_ktable)->spawn(transform2_at(kShipStart), Local2{}, Parent{ship}, dot);
    beam_spawn(e, g, ship);
    engine_entity_name_set(e, ship, "ship");
    return ship;
}

// Phase Sim: the actions fly the ship and fire. While the egg plays the ship is not live
// and nothing here moves it.
void control(TickContext& ctx) {
    static const u32 k_actions_id = ctx.resource_id("input.actions");
    Game& g = ctx.state_as<Game>();
    const InputActions& a = *ctx.resource<const InputActions>(k_actions_id);
    Span<Transform2> xf = ctx.write<Transform2>("arcfire.ship.Transform2");
    Span<Ship> ships = ctx.write<Ship>("arcfire.ship.Ship");
    Span<Sprite> ring = ctx.write<Sprite>("arcfire.ring.Sprite");
    Span<Sprite> mark = ctx.write<Sprite>("arcfire.mark.Sprite");
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

    s.firing = action_down(a, g.act_shot) ? 1 : 0;
    if (!s.firing || s.cooldown) return;
    KTable& shots_table = *ctx.write_ktable(g_shot_ktable);
    s.cooldown = weapon_fire(g, shots_table, p, s.weapon, s.power, focus);
    // The options fire with it, two levels lower, from where they were as the tick began.
    KTableRows options = ctx.rows(g_option_ktable);
    Span<const Transform2> ox = ctx.read<Transform2>("arcfire.option.Transform2");
    for (u32 k = 0; k < options.rows(); ++k)
        if (options.alive(k)) weapon_fire(g, shots_table, ox[k].position, s.weapon, option_power(s.power), focus);
    sfx(ctx, g, SFX_SHOT);
    ++s.shots;
}

} // namespace kv::arcfire
