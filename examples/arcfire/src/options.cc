// arcfire: options. Up to two satellites, children of the ship. Unfocused they orbit
// it, opposite each other, a turn every two seconds; focused they hold beside it, left and
// right, so the player aims them, easing between the two. Each fires the ship's weapon two
// levels lower on the ship's volley (control, ship.cc), its own beam when the weapon is the
// Beam (weapons.cc), and is a body on the shell's layer: a bullet that meets it is eaten,
// as the egg's shell eats them. An option item adds one; a death takes both.
#include "arcfire.h"
#include "korvid/math/scalar.h"

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
    KTable* options = ctx.write_ktable(g_option_ktable);
    Span<const Option> oo = options->rows_of<Option>();
    bool taken[kMaxOptions] = {};
    u32 have = 0;
    for (u32 i = 0; i < options->rows(); ++i)
        if (options->is_alive(i) && oo[i].index < kMaxOptions) { taken[oo[i].index] = true; ++have; }
    if (have >= kMaxOptions || g.ship.is_null()) return false;
    u32 index = taken[0] ? 1 : 0;
    // A small spherical machine, as the egg is, tinted to the shots' cyan.
    Sprite s = sprite_of(g, TEX_EGG, rect2_unit(), vec2{22, 22}, Z_SHIP - 1, color{0.75f, 0.95f, 1.0f, 1.0f});
    point2 at = engine_ktable(*ctx.engine, g_ship_ktable)->get<Transform2>(g.ship)->position;
    Handle h = options->spawn(transform2_at(at), local2_at(point2{0, 0}), Parent{g.ship}, s, option_collider(), Option{index, 0.0f});
    if (!h.is_null()) beam_spawn(*ctx.engine, g, h);
    return !h.is_null();
}

// Phase Sim, after control (which reads where the options were) and before the collision
// module composes them: each option's place on the ship; its beams' lean, which eases after
// the ship's sideways movement this tick, so they sweep the way it goes.
void options(TickContext& ctx) {
    const Game& g = ctx.state_as<const Game>();
    const KTable* ships = engine_ktable(*ctx.engine, g_ship_ktable);
    const Ship* s = ships->get<Ship>(g.ship);
    const Transform2* sx = ships->get<Transform2>(g.ship);
    const Transform2* sp = ships->prev<Transform2>(g.ship);
    if (!s || !sx || !sp) return;
    f32 sideways = clampf((sx->position.x - sp->position.x) / kShipSpeed, -1.0f, 1.0f);
    Span<Local2> ol = ctx.write<Local2>("arcfire.option.Local2");
    Span<Option> oo = ctx.write<Option>("arcfire.option.Option");
    const KTable* options = engine_ktable(*ctx.engine, g_option_ktable);
    f32 turn = (f32)ctx.tick * (2.0f * KV_PI / kOrbitTicks);
    for (u32 i = 0; i < options->rows(); ++i) {
        if (!options->is_alive(i)) continue;
        f32 side = oo[i].index == 0 ? -1.0f : 1.0f;
        point2 want = s->focus ? point2{side * kHold.x, kHold.y}
                               : point2{kv::cos(turn + KV_PI * (f32)oo[i].index) * kOrbit, kv::sin(turn + KV_PI * (f32)oo[i].index) * kOrbit};
        ol[i].position += (want - ol[i].position) * kEase;
        oo[i].tilt += (sideways * kTiltMost - oo[i].tilt) * kTiltEase;
    }
}

// ---- what this file declares (its part of the module: module.cc lists it) -----------------------
namespace options_part {
constinit const FieldDesc k_option_fields[] = {KV_FIELDS(Option, index, tilt)};
constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(Option, k_option_fields),
};
constinit const KTableDesc k_ktables[] = {
    // Explosions: a flipbook each, gone when it has played (fx.cc).
    // The beams: one on the ship (and one on each option), cut each tick at what it meets.
    // Options: children of the ship, bodies that eat bullets (options.cc).
    {.name = ".option",   .components = "Transform2, Local2, Parent, Sprite, Collider2, Option", .capacity = kMaxOptions, .id = &g_option_ktable},
};
constinit const SystemDesc k_systems[] = {
    // The beam is cut against everything where it is this tick: after every mover and the
    // collision module's compose.
    // Each option's place on the ship, before the collision module composes it.
    {.name = ".options", .phase = Phase::Sim, .fn = options, .reads = ".ship.Ship, .ship.Transform2", .writes = ".option.Local2, .option.Option", .after = ".control",
     .state = StateUse::Read},
};
} // namespace options_part
KV_MODULE_PART(arcfire_options) = {
    .components = options_part::k_components,
    .ktables    = options_part::k_ktables,
    .systems    = options_part::k_systems,
};

} // namespace kv::arcfire
