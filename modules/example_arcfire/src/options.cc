// example_arcfire: options. Up to two satellites, children of the ship. Unfocused they orbit
// it, opposite each other, a turn every two seconds; focused they hold beside it, left and
// right, so the player aims them, easing between the two. Each fires the ship's weapon two
// levels lower on the ship's volley (control, ship.cc), its own beam when the weapon is the
// Beam (weapons.cc), and is a body on the shell's layer: a bullet that meets it is eaten,
// as the egg's shell eats them. An option item adds one; a death takes both.
#include "arcfire.h"
#include <math.h>

namespace kv::arcfire {

namespace {

constexpr f32 kOrbit = 44.0f;          // units from the ship, unfocused
constexpr f32 kOrbitTicks = 120.0f;    // a turn
constexpr point2 kHold{40.0f, -6.0f};  // focused: this far to either side, a little behind
constexpr f32 kEase = 0.25f;           // how fast an option goes where it is wanted
constexpr f32 kTiltMost = 12.0f * KV_PI / 180.0f; // its beams' lean at the ship's full sideways speed
constexpr f32 kTiltEase = 0.12f;       // how fast the lean follows
constexpr f32 kShipSpeed = 6.0f;       // ship.cc's, unfocused

Collider2 option_collider() { return collider_circle(10.0f, L_SHELL, L_BULLET); }

} // namespace

bool option_add(TickContext& ctx, Game& g) {
    Table* options = ctx.write_kind(g_option_kind);
    Span<const Option> oo = options->rows_of<Option>();
    bool taken[kMaxOptions] = {};
    u32 have = 0;
    for (u32 i = 0; i < options->rows(); ++i)
        if (options->is_alive(i) && oo[i].index < kMaxOptions) { taken[oo[i].index] = true; ++have; }
    if (have >= kMaxOptions || g.ship.is_null()) return false;
    u32 index = taken[0] ? 1 : 0;
    // A small spherical machine, as the egg is, tinted to the shots' cyan.
    Sprite s = sprite_of(g, TEX_EGG, rect2_unit(), vec2{22, 22}, Z_SHIP - 1, color{0.75f, 0.95f, 1.0f, 1.0f});
    point2 at = engine_table(*ctx.engine, g_ship_kind)->get<Transform2>(g.ship)->position;
    Handle h = options->spawn(transform2_at(at), local2_at(point2{0, 0}), Parent{g.ship}, s, option_collider(), Option{index, 0.0f});
    if (!h.is_null()) beam_spawn(*ctx.engine, g, h);
    return !h.is_null();
}

// Phase Sim, after control (which reads where the options were) and before the collision
// module composes them: each option's place on the ship; its beams' lean, which eases after
// the ship's sideways movement this tick, so they sweep the way it goes.
void options(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    const Table* ships = engine_table(*ctx.engine, g_ship_kind);
    const Ship* s = ships->get<Ship>(g.ship);
    const Transform2* sx = ships->get<Transform2>(g.ship);
    const Transform2* sp = ships->prev<Transform2>(g.ship);
    if (!s || !sx || !sp) return;
    f32 sideways = clampf((sx->position.x - sp->position.x) / kShipSpeed, -1.0f, 1.0f);
    Span<Local2> ol = ctx.write<Local2>("example_arcfire.option.Local2");
    Span<Option> oo = ctx.write<Option>("example_arcfire.option.Option");
    const Table* options = engine_table(*ctx.engine, g_option_kind);
    f32 turn = (f32)ctx.tick * (2.0f * KV_PI / kOrbitTicks);
    for (u32 i = 0; i < options->rows(); ++i) {
        if (!options->is_alive(i)) continue;
        f32 side = oo[i].index == 0 ? -1.0f : 1.0f;
        point2 want = s->focus ? point2{side * kHold.x, kHold.y}
                               : point2{cosf(turn + KV_PI * (f32)oo[i].index) * kOrbit, sinf(turn + KV_PI * (f32)oo[i].index) * kOrbit};
        ol[i].position += (want - ol[i].position) * kEase;
        oo[i].tilt += (sideways * kTiltMost - oo[i].tilt) * kTiltEase;
    }
}

} // namespace kv::arcfire
