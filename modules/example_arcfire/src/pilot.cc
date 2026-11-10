// example_arcfire: who flies the ship when the player does not. Both drive the action map
// through input_action_override, so the ship is flown by the same code whoever holds the
// stick, and both steer by one rule (`steer`, below) from sim state only.
//
// The rule: for every bullet, where it will pass closest in the next 40 ticks; away from
// each pass that comes within `clear` units, the sooner and the closer the harder; away from any air enemy within 90; off the walls; and with nothing
// to fear, under the nearest enemy (so the shots find it) at the ship's usual height. It
// is not good, which is what the stress mode wants (the ship cannot die there), and enough
// to live through most of a minute at Normal.
//
// The demo (--demo) runs first in Input, so its actions are this tick's: fire throughout,
// a few scripted moves at the start (across the field, focused through the first
// formation's stream so the ring grazes), the rule otherwise, and from a known tick it
// stands still until a bullet finds it (the egg plays once). The stage's end mark quits
// it. The bot (--autoplay, --stress) is a Sim system; its override lands on the next
// tick's sample.
#include "arcfire.h"
#include "korvid/platform_hid/actions.h"
#include <math.h>

namespace kv::arcfire {

namespace {

struct Step { u64 from, to; const char* action; f32 x, y; };
constinit const Step k_script[] = {
    {20, 70, "move", -1, 0},       // over to the left
    {70, 100, "move", 0, 1},       // and up, toward the first formation
    {180, 300, "focus", 0, 0},     // focused: the ring shows, the ship slows through the stream
};
constexpr u64 kScriptTo = 300;     // from here the rule flies
constexpr u64 kStandFrom = 1500;   // the demo stops dodging until it has died once
constexpr f32 kClear = 26.0f;      // the bot's margin: clear of the ring
constexpr f32 kGrazeClear = 11.0f; // the demo's, focused: inside the ring, outside the hitbox

// The rule, from where everything stood when the last tick ended. A bullet whose line
// passes wider than `clear` is let by: with a small clearance the ring grazes it.
vec2 steer(Engine& e, const Game& g, f32 clear) {
    const Table* ships = engine_table(e, g_ship_kind);
    const Transform2* sx = ships->get<Transform2>(g.ship);
    if (!sx) return vec2{0, 0};
    point2 p = sx->position;
    // Each bullet's closest pass in the next kAhead ticks, if it comes within `clear`: away
    // from that point, the sooner and the closer the harder.
    constexpr f32 kAhead = 40.0f;
    vec2 push{0, 0};
    const u32 kinds[] = {g_bullet_kind, g_active_kind};
    for (u32 kind : kinds) {
        const Table* bullets_table = engine_table(e, kind);
        Span<const Transform2> bt = bullets_table->rows_of<Transform2>();
        Span<const Velocity> bv = bullets_table->rows_of<Velocity>();
        for (u32 i = 0; i < bullets_table->rows(); ++i) {
            if (!bullets_table->is_alive(i)) continue;
            vec2 rel = bt[i].position - p;
            if (length_sq(rel) > 200.0f * 200.0f) continue;
            vec2 v = bv[i].v;
            f32 vv = length_sq(v);
            f32 t = vv > 1e-6f ? -dot(rel, v) / vv : 0.0f; // ticks to its closest pass
            if (t < 0.0f) t = 0.0f;
            if (t > kAhead) continue;
            vec2 closest = rel + v * t;                   // where it will be then, from the ship
            f32 dist = length(closest);
            if (dist > clear) continue;
            vec2 away = dist > 0.01f ? closest * (-1.0f / dist) : perp(normalize(v));
            push += away * ((1.0f - dist / clear) * (1.0f - t / kAhead));
        }
    }
    const Table* air = engine_table(e, g_air_kind);
    Span<const Transform2> at = air->rows_of<Transform2>();
    f32 nearest = 1e9f, aim_x = 0.0f;
    for (u32 i = 0; i < air->rows(); ++i) {
        if (!air->is_alive(i)) continue;
        vec2 d = p - at[i].position;
        f32 dist = length(d);
        if (dist < 90.0f && dist > 0.01f) push += d * ((1.0f - dist / 90.0f) / dist);
        if (at[i].position.y > p.y + 60.0f && in_field(at[i].position, -20.0f) && fabsf(d.x) < nearest) { nearest = fabsf(d.x); aim_x = at[i].position.x; }
    }
    const Table* ground = engine_table(e, g_ground_kind);
    Span<const Transform2> gt = ground->rows_of<Transform2>();
    for (u32 i = 0; i < ground->rows(); ++i)
        if (ground->is_alive(i) && gt[i].position.y > p.y + 60.0f && fabsf(gt[i].position.x - p.x) < nearest) {
            nearest = fabsf(gt[i].position.x - p.x);
            aim_x = gt[i].position.x;
        }
    // The walls push back, so a dodge does not end in a corner.
    if (p.x < kFieldLeft + 80.0f) push.x += 0.5f;
    if (p.x > kFieldRight - 80.0f) push.x -= 0.5f;
    if (p.y < kFieldBottom + 70.0f) push.y += 0.5f;
    if (p.y > -60.0f) push.y -= 0.5f;
    if (length_sq(push) > 1e-4f) return normalize(push);
    f32 goal_x = nearest < 1e9f ? aim_x : 0.0f;
    return vec2{clampf((goal_x - p.x) / 60.0f, -1.0f, 1.0f), clampf((kShipStart.y - p.y) / 60.0f, -1.0f, 1.0f)};
}

} // namespace

// Phase Input, before the sample, every sim tick.
void demo(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    if (!g.demo || !g.stage_ready) return;
    Engine& e = *ctx.engine;
    u64 t = ctx.tick;
    input_action_override(e, "shot", true);
    for (const Step& s : k_script)
        if (t >= s.from && t < s.to) input_action_override(e, s.action, true, vec2{s.x, s.y});
    if (t < kScriptTo) {
        if (t >= 100) input_action_override(e, "move", true, steer(e, g, kGrazeClear)); // close enough to graze
        return;
    }
    const Run* r = engine_table(e, g_world_kind)->get<Run>(g.world);
    if (t >= kStandFrom && r && r->deaths == 0) return; // standing in the stream
    input_action_override(e, "move", true, steer(e, g, kClear));
}

// Phase Sim (early reads: where everything stood at the tick's start).
void bot(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Span<const Dials> dials = ctx.read<Dials>(ARC_WORLD(Dials));
    if (!dials.len || !dials[0].bot) return;
    input_action_override(*ctx.engine, "move", true, steer(*ctx.engine, g, kClear));
    input_action_override(*ctx.engine, "shot", true);
}

} // namespace kv::arcfire
