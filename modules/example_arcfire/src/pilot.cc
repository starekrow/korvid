// example_arcfire: who flies the ship when the player does not. Both drive the action map
// through input_action_override, so the ship is flown by the same code whoever holds the
// stick, and both steer by one rule (`steer`, below) from sim state only.
//
// The rule: for every bullet, where it will pass closest in the next 40 ticks; away from
// each pass that comes within `clear` units, the sooner and the closer the harder; away from
// any air enemy within 90 and from a boss's hull; off the walls; and with nothing to fear,
// under the nearest enemy (so the shots find it) at the ship's usual height, or, with a
// boss up, beside the lowest of its open parts that end its phase (any open part, if none
// can be reached) by as much as the spread opens at that height, so one of the two shots
// goes through it, focused once it is there. With nothing to fear and an item worth having
// in the field, it goes for the item first: power, options, eggs, bombs, and a weapon item
// while it shows a type the pilot wants (one it does not want it keeps clear of, since
// taking it would switch). The bot wants Lance or Storm at a boss (the Beam covers the most
// and hurts any one target least) and Spread or Storm otherwise; the demo
// wants what its script says. With the arsenal it clears Shoreline at every difficulty
// (the notebook has its record), more by the weapons than by its dodging.
//
// The bot bombs to live: when a bullet will meet the hitbox before the push can clear it,
// or bullets close in from every side, or the field crowds in. From a boss's last ten
// seconds it keeps one bomb back for the stomp.
//
// The demo (--demo) runs first in Input, so its actions are this tick's: fire throughout,
// a few scripted moves at the start (across the field, focused through the first
// formation's stream so the ring grazes), the rule otherwise, and from a known tick it
// stands still and holds fire until a bullet finds it (the egg plays once); a bomb once the
// carrier's core has opened; the battleship fought until it dies or leaves. Its weapons by
// the stage's clock: Spread on the opening's power items, the Beam at the islands, Storm
// from the airfield on, Lance for the battleship; the options where the bunkers drop them.
// The stage's end mark quits it. Orbit's demo (--demo --stage orbit) has no script: the
// rule throughout, its own weapons by the stage's clock, a bomb when the station's core
// opens. The bot (--autoplay, --stress) is a Sim system; its override
// lands on the next tick's sample.
#include "arcfire.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/platform_hid/actions.h"
#include <math.h>
#include <string.h>

namespace kv::arcfire {

namespace {

struct Step { u64 from, to; const char* action; f32 x, y; };
constinit const Step k_script[] = {
    {20, 70, "move", -1, 0},       // over to the left
    {70, 100, "move", 0, 1},       // and up, toward the first formation
    {180, 300, "focus", 0, 0},     // focused: the ring shows, the ship slows through the stream
    {1440, 1441, "bomb", 0, 0},    // in the spiral: a bomb, the field to chips
    {1442, 1530, "move", 0, 1},    // up past the magnet line: every chip comes
};
constexpr u64 kScriptTo = 300;     // from here the rule flies (but for the steps above)
constexpr u64 kBombFrom = 1442, kBombTo = 1530; // the climb after the bomb, guarded
constexpr u64 kStandFrom = 1650;   // the demo stops dodging until it has died once
constexpr u32 kCrowd = 40;         // the bot bombs when this many bullets close in
constexpr u32 kBossCrowd = 25;     // or this many, at a boss
constexpr f32 kClear = 26.0f;      // the bot's margin: clear of the ring
constexpr f32 kGrazeClear = 11.0f; // the demo's, focused: inside the ring, outside the hitbox
constexpr f32 kHit = 3.0f + 4.0f + 1.5f; // the hitbox's radius and a bullet's, and a little
constexpr f32 kStep = 6.0f, kFocusStep = 3.0f; // the ship's speeds (ship.cc)
constexpr f32 kKeep = 0.6f;        // how much of the last move a dodge keeps: two passes alike do not dither
constexpr f32 kSeek = 260.0f;      // the plainer items are gone for this close
constexpr f32 kShun = 80.0f;       // an unwanted weapon item is kept this far off (its reach is 40)
constexpr u32 kKeepFor = 600;      // a boss's last ticks before its stomp: one bomb kept back

constexpr u32 bit(u32 weapon) { return 1u << weapon; }

// The demo's weapons, by the stage's clock: Spread on the opening, the Beam at the
// islands, Storm from the last of them over the airfield and the carrier, Lance for the
// battleship.
struct Want { f32 from; u32 weapons; };
constinit const Want k_demo_wants[] = {
    {0.0f, bit(WeaponSpread)}, {4300.0f, bit(WeaponBeam)}, {5800.0f, bit(WeaponStorm)}, {14000.0f, bit(WeaponLance)},
};
// Orbit's (--demo --stage orbit): Spread through the opening's popcorn and debris, Storm
// for the segments' turrets and the gunships, Lance for the station core.
constinit const Want k_orbit_wants[] = {
    {0.0f, bit(WeaponSpread)}, {3000.0f, bit(WeaponStorm)}, {15000.0f, bit(WeaponLance)},
};

// What the steering decided: the stick, focus, and whether the hitbox is about to be hit
// whatever the stick does (the bot's cue to bomb).
struct Plan { vec2 move{0, 0}; bool focus = false; bool trapped = false; };

// The rule, from where everything stood when the last tick ended and the way the ship is
// going (the last stick: a bullet the ship is running into is as close as one coming at
// it). A bullet whose line passes wider than `clear` is let by: with a small clearance the
// ring grazes it. `want` is the weapons the pilot would switch to (bits).
Plan steer_rule(Engine& e, const Game& g, f32 clear, u32 want) {
    Plan plan;
    const Table* ships = engine_table(e, g_ship_kind);
    const Transform2* sx = ships->get<Transform2>(g.ship);
    const Ship* ship = ships->get<Ship>(g.ship);
    if (!sx || !ship) return plan;
    point2 p = sx->position;
    // Each bullet's closest pass in the next kAhead ticks, if it comes within `clear`: away
    // from that point, the sooner and the closer the harder.
    constexpr f32 kAhead = 40.0f;
    vec2 push{0, 0};
    // The threats: bullets that will meet the hitbox in the next few ticks. One the stick
    // cannot outrun, or several from sides that cancel, is a trap.
    vec2 threat_sum{0, 0};
    u32 threats = 0;
    const u32 kinds[] = {g_bullet_kind, g_active_kind};
    for (u32 kind : kinds) {
        const Table* bullets_table = engine_table(e, kind);
        Span<const Transform2> bt = bullets_table->rows_of<Transform2>();
        Span<const Velocity> bv = bullets_table->rows_of<Velocity>();
        for (u32 i = 0; i < bullets_table->rows(); ++i) {
            if (!bullets_table->is_alive(i)) continue;
            vec2 rel = bt[i].position - p;
            if (length_sq(rel) > 200.0f * 200.0f) continue;
            vec2 v = bv[i].v - g.pilot_v;
            f32 vv = length_sq(v);
            f32 t = vv > 1e-6f ? -dot(rel, v) / vv : 0.0f; // ticks to its closest pass
            if (t < 0.0f) t = 0.0f;
            if (t > kAhead) continue;
            vec2 closest = rel + v * t;                   // where it will be then, from the ship
            f32 dist = length(closest);
            if (dist > clear) continue;
            vec2 away = dist > 0.01f ? closest * (-1.0f / dist) : perp(normalize(v));
            push += away * ((1.0f - dist / clear) * (1.0f - t / kAhead));
            if (dist < kHit && t < 6.0f) {
                // The stick lands a tick late, and the push is a mix, not a clean step
                // aside: a hit three ticks off is a hit.
                if (t < 3.0f) plan.trapped = true;
                threat_sum += away;
                ++threats;
            }
        }
    }
    if (threats >= 3 && length(threat_sum) < 0.5f * (f32)threats) plan.trapped = true;
    // Items: the nearest worth having that has not fallen past; an unwanted weapon kept off.
    const Table* items_table = engine_table(e, g_item_kind);
    Span<const Transform2> ix = items_table->rows_of<Transform2>();
    Span<const Item> iv = items_table->rows_of<Item>();
    f32 item_d = 1e9f;
    point2 item_at{0, 0};
    bool item_prized = false; // a weapon the pilot wants, an option, an egg: worth flying up the field for
    for (u32 i = 0; i < items_table->rows(); ++i) {
        if (!items_table->is_alive(i) || iv[i].kind == ItemChip || !in_field(ix[i].position, -10.0f)) continue;
        vec2 d = ix[i].position - p;
        f32 dist = length(d);
        if (iv[i].kind == ItemWeapon) {
            u32 shown = weapon_shown(iv[i]);
            // The type it shows is wanted, or is the ship's own while that can still power up;
            // else taking it would switch for nothing, or waste it.
            if (!(want & bit(shown)) && !(shown == ship->weapon && ship->power < kMaxPower)) {
                // Inside its reach it comes to the ship faster than the ship can leave.
                if (dist < kShun) push.x += (d.x > 0.0f ? -1.0f : 1.0f) * (1.0f - dist / kShun) * (dist < kShun * 0.75f ? 3.0f : 1.0f);
                continue;
            }
        }
        bool prized = iv[i].kind == ItemWeapon || iv[i].kind == ItemOption || iv[i].kind == ItemEgg;
        f32 rank = iv[i].kind == ItemWeapon ? dist * 0.5f : prized ? dist * 0.75f : dist; // a weapon, then the prized
        if (d.y > -10.0f && (prized || dist < kSeek) && rank < item_d) { item_d = rank; item_at = ix[i].position; item_prized = prized; }
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
    // A boss: the lowest of its open parts that end its phase is the target, ahead of
    // anything else (the lowest open part, if none of those can be reached); its hull is to
    // be kept off.
    const Table* boss_table = engine_table(e, g_boss_kind);
    const Table* parts = engine_table(e, g_part_kind);
    Span<const Transform2> px = parts->rows_of<Transform2>();
    Span<const Part> pp = parts->rows_of<Part>();
    Span<const Health> ph = parts->rows_of<Health>();
    f32 lowest = 1e9f;
    bool ends_phase = false, at_part = false;
    for (u32 i = 0; i < parts->rows(); ++i) {
        if (!parts->is_alive(i) || ph[i].hp <= 0) continue;
        const Boss* b = boss_table->get<Boss>(pp[i].boss);
        if (!b || (b->state != (u32)BossState::Holding && b->state != (u32)BossState::Stomping)) continue;
        const BossDef& def = g.bosses[b->def];
        const PartDef& pd = def.parts[pp[i].index];
        point2 pos = px[i].position;
        if (pd.armored_until > b->phase || pos.y < p.y + 60.0f) continue;
        bool ends = (def.phases[b->phase].ends >> pp[i].index) & 1u;
        if ((ends_phase && !ends) || (ends == ends_phase && pos.y >= lowest)) continue;
        lowest = pos.y;
        ends_phase = ends;
        // Spread's innermost shot leaves beside the nose and opens by its angle: at the
        // part's height it is this far out, so stand that far to whichever side is nearer
        // (focused, as the ship will be once it is there). The Beam, a lance and Storm's
        // bolts go where the ship is.
        f32 off = 0.0f;
        if (ship->weapon == WeaponSpread) {
            f32 mx = 0.0f, ang = 0.0f;
            spread_inner(ship->power, true, &mx, &ang);
            off = mx + (pos.y - p.y - kMuzzleUp) * tanf(ang);
        }
        aim_x = fabsf(pos.x - off - p.x) < fabsf(pos.x + off - p.x) ? pos.x - off : pos.x + off;
        nearest = 0.0f;
        at_part = true;
    }
    Span<const Transform2> bx = boss_table->rows_of<Transform2>();
    Span<const Collider2> bc = boss_table->rows_of<Collider2>();
    for (u32 i = 0; i < boss_table->rows(); ++i) {
        if (!boss_table->is_alive(i)) continue;
        f32 below = (bx[i].position.y + bc[i].offset.y - bc[i].half.y) - p.y; // from the ship up to the hull's lowest edge
        if (below < 70.0f && fabsf(bx[i].position.x - p.x) < bc[i].half.x + 40.0f) push.y -= 1.0f - (below > 0.0f ? below / 70.0f : 0.0f);
    }
    if (length_sq(push) > 1e-4f) {
        // The walls push back, so a dodge does not end in a corner.
        if (p.x < kFieldLeft + 80.0f) push.x += 0.5f;
        if (p.x > kFieldRight - 80.0f) push.x -= 0.5f;
        if (p.y < kFieldBottom + 70.0f) push.y += 0.5f;
        if (p.y > -60.0f) push.y -= 0.5f;
        vec2 kept = push + g.pilot_v * (kKeep / kStep);
        plan.move = normalize(length_sq(kept) > 1e-4f ? kept : push);
        return plan;
    }
    // Nothing to fear: an item first, met a little below it (a prized one as high as a
    // third of the way under the magnet line, a weapon while it shows a type wanted; the
    // rest in the lower half); else the target, focused on a part once under it.
    if (item_d < 1e9f) {
        f32 goal_y = clampf(item_at.y - 40.0f, kShipStart.y, item_prized ? kMagnetY - 120.0f : -80.0f);
        plan.move = vec2{clampf((item_at.x - p.x) / 30.0f, -1.0f, 1.0f), clampf((goal_y - p.y) / 60.0f, -1.0f, 1.0f)};
        return plan;
    }
    f32 goal_x = nearest < 1e9f ? aim_x : 0.0f;
    plan.move = vec2{clampf((goal_x - p.x) / 60.0f, -1.0f, 1.0f), clampf((kShipStart.y - p.y) / 60.0f, -1.0f, 1.0f)};
    plan.focus = at_part && fabsf(goal_x - p.x) < 6.0f;
    return plan;
}

Plan steer(Engine& e, Game& g, f32 clear, u32 want) {
    Plan plan = steer_rule(e, g, clear, want);
    g.pilot_v = plan.move * (plan.focus ? kFocusStep : kStep);
    return plan;
}

// Whether a boss is up; `keep`, whether one is in its last ticks before the stomp (a bomb
// is kept back for it).
bool boss_up(Engine& e, const Game& g, bool* keep) {
    const Table* bt = engine_table(e, g_boss_kind);
    Span<const Boss> bb = bt->rows_of<Boss>();
    bool up = false;
    *keep = false;
    for (u32 i = 0; i < bt->rows(); ++i) {
        if (!bt->is_alive(i)) continue;
        up = true;
        if (bb[i].state == (u32)BossState::Holding && bb[i].clock + kKeepFor >= g.bosses[bb[i].def].timeout) *keep = true;
    }
    return up;
}

void fly(Engine& e, const Plan& plan) {
    input_action_override(e, "move", true, plan.move);
    if (plan.focus) input_action_override(e, "focus", true);
}

// A pilot holds every gameplay action, every tick: what it does not press is held up, so
// a key or a pad's stick touched while it flies changes nothing (the run stays the run).
void release_all(Engine& e) {
    input_action_override(e, "move", false);
    input_action_override(e, "shot", false);
    input_action_override(e, "focus", false);
    input_action_override(e, "bomb", false);
}

} // namespace

// Phase Input, before the sample, every sim tick.
void demo(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    if (!g.demo || !g.stage_ready || frameloop_replaying(*ctx.engine)) return; // a replay flies it
    Engine& e = *ctx.engine;
    u64 t = ctx.tick;
    release_all(e);
    if (g.stage_at != 0) { // Orbit's demo: no script, the rule throughout, a bomb when the core opens
        input_action_override(e, "shot", true);
        if (!g.demo_core_bomb) {
            const Table* bt = engine_table(e, g_boss_kind);
            Span<const Boss> bb = bt->rows_of<Boss>();
            for (u32 i = 0; i < bt->rows(); ++i)
                if (bt->is_alive(i) && !strcmp(g.bosses[bb[i].def].name, "station") && bb[i].phase + 1 == g.bosses[bb[i].def].phase_count &&
                    bb[i].state == (u32)BossState::Holding) {
                    input_action_override(e, "bomb", true);
                    g.demo_core_bomb = true;
                }
        }
        const Stage* os = engine_table(e, g_world_kind)->get<Stage>(g.world);
        u32 want = bit(WeaponSpread);
        for (const Want& w : k_orbit_wants)
            if (os && stage_clock(*os) >= w.from) want = w.weapons;
        fly(e, steer(e, g, kClear, want));
        return;
    }
    const Run* r = engine_table(e, g_world_kind)->get<Run>(g.world);
    bool standing = t >= kStandFrom && r && r->deaths == 0; // holding fire, so the enemies live to shoot
    if (!standing) input_action_override(e, "shot", true);
    for (const Step& s : k_script)
        if (t >= s.from && t < s.to) input_action_override(e, s.action, true, vec2{s.x, s.y});
    if (!g.demo_core_bomb) { // the carrier's core open: a bomb, once
        const Table* bt = engine_table(e, g_boss_kind);
        Span<const Boss> bb = bt->rows_of<Boss>();
        for (u32 i = 0; i < bt->rows(); ++i) {
            if (!bt->is_alive(i)) continue;
            const BossDef& def = g.bosses[bb[i].def];
            if (!strcmp(def.name, "carrier") && bb[i].phase + 1 == def.phase_count && bb[i].state == (u32)BossState::Holding) {
                input_action_override(e, "bomb", true);
                g.demo_core_bomb = true;
            }
        }
    }
    const Table* world = engine_table(e, g_world_kind);
    const Stage* st = world->get<Stage>(g.world);
    u32 want = bit(WeaponSpread);
    for (const Want& w : k_demo_wants)
        if (st && stage_clock(*st) >= w.from) want = w.weapons;
    if (t < kScriptTo) {
        if (t >= 100) input_action_override(e, "move", true, steer(e, g, kGrazeClear, want).move); // close enough to graze
        return;
    }
    if (t >= kBombFrom && t < kBombTo) return; // the climb, scripted above
    if (standing) return;                      // standing in the stream
    fly(e, steer(e, g, kClear, want));
}

// Phase Sim (early reads: where everything stood at the tick's start).
void bot(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Span<const Dials> dials = ctx.read<Dials>(ARC_WORLD(Dials));
    if (!dials.len || !dials[0].bot || frameloop_replaying(*ctx.engine)) return; // a replay flies it
    Engine& e = *ctx.engine;
    release_all(e);
    bool keep = false;
    Plan plan = steer(e, g, kClear, boss_up(e, g, &keep) ? bit(WeaponLance) | bit(WeaponStorm) : bit(WeaponSpread) | bit(WeaponStorm));
    fly(e, plan);
    input_action_override(e, "shot", true);
    // A bomb, to live: when the hitbox is about to be hit whatever the stick does, or the
    // bullets crowd in; one kept back for a stomp to come (not under stress: the ship
    // cannot die there, and the stress wants the bullets).
    const Run* r = engine_table(e, g_world_kind)->get<Run>(g.world);
    const Table* ships = engine_table(e, g_ship_kind);
    const Ship* s = ships->get<Ship>(g.ship);
    const Transform2* sx = ships->get<Transform2>(g.ship);
    if (dials[0].invulnerable == 1 || !r || r->bombs <= (keep ? 1 : 0) || !s || !s->live || s->guard || !sx) return;
    if (plan.trapped) {
        input_action_override(e, "bomb", true);
        return;
    }
    u32 close = 0;
    const u32 kinds[] = {g_bullet_kind, g_active_kind};
    for (u32 kind : kinds) {
        const Table* t = engine_table(e, kind);
        Span<const Transform2> bt = t->rows_of<Transform2>();
        Span<const Velocity> bv = t->rows_of<Velocity>();
        for (u32 i = 0; i < t->rows() && close <= kCrowd; ++i) { // past kCrowd is past either
            if (!t->is_alive(i)) continue;
            vec2 d = sx->position - bt[i].position;
            if (length_sq(d) < 80.0f * 80.0f && dot(bv[i].v, d) > 0.0f) ++close;
        }
    }
    if (close > (engine_table(e, g_boss_kind)->live() ? kBossCrowd : kCrowd)) input_action_override(e, "bomb", true);
}

} // namespace kv::arcfire
