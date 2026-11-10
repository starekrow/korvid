// example_arcfire: the weapons. Four types, each at five power levels, as rows of the
// tables below (the slice 4 PRD's starting numbers; the closing tuning pass settles them):
//
//   Spread  a fan of shots from the nose, more and wider with power, narrowed when focused
//   Beam    continuous, locked to the ship: cut each tick at the first body it meets, which
//           it damages every few ticks; wider and harder with power; focused, half as wide
//           and a third harder. The one weapon a boss's hull stops
//   Storm   bolts that turn toward the nearest enemy, more a volley with power
//   Lance   a heavy, slow shot that passes through what it kills and stops at what it does not
//
// The level is the ship's, kept when the type changes. Shots are one kind (`shot`); a bolt
// steers in `shots`, a lance pierces in the hit (hits.cc). The beam is a child row of the
// ship (`beam`), shown while the shot is held with the Beam in hand; `beams` casts it.
#include "arcfire.h"
#include "korvid/core/memory.h"
#include <math.h>

namespace kv::arcfire {

namespace {

constexpr f32 kDeg = KV_PI / 180.0f;
constexpr f32 kShotSpeed = 14.0f;
constexpr f32 kBoltSpeed = 8.0f;
constexpr f32 kFocusFan = 0.35f;     // Spread's fan, focused, against unfocused
constexpr f32 kBeamReach = 760.0f;   // past the field's top from anywhere in it
constexpr f32 kBeamStrip = 64.0f;    // world units of the beam's texture before it repeats
constexpr f32 kBeamScroll = 0.08f;   // strips a tick, up the beam
constexpr f32 kBeamFocusWidth = 0.5f, kBeamFocusDamage = 4.0f / 3.0f;

struct SpreadLevel { u32 shots; f32 fan; u32 every; i32 damage; };
constinit const SpreadLevel k_spread[kMaxPower] = {{2, 6, 6, 1}, {3, 10, 6, 1}, {4, 14, 5, 1}, {5, 18, 5, 1}, {7, 24, 5, 1}};
struct BeamLevel { f32 width; i32 damage; u32 every; };
constinit const BeamLevel k_beam[kMaxPower] = {{6, 1, 4}, {10, 1, 3}, {16, 2, 4}, {24, 2, 3}, {36, 3, 3}};
struct StormLevel { u32 bolts; u32 every; f32 turn; i32 damage; };
constinit const StormLevel k_storm[kMaxPower] = {{1, 8, 6, 1}, {2, 8, 7, 1}, {2, 6, 8, 1}, {3, 6, 9, 1}, {4, 6, 10, 1}};
struct LanceLevel { f32 size; u32 every; f32 speed; i32 damage; };
constinit const LanceLevel k_lance[kMaxPower] = {{20, 16, 9, 5}, {24, 15, 9, 7}, {28, 14, 10, 9}, {34, 13, 10, 12}, {40, 12, 11, 16}};

u32 level(u32 power) { return (power < 1 ? 1 : power > kMaxPower ? kMaxPower : power) - 1; }

// A shot going `ang` radians clockwise from up at `speed`, its sprite turned to match.
void shot_spawn(Table& shots, const Sprite& s, point2 at, f32 ang, f32 speed, f32 radius, const Shot& shot) {
    vec2 v{sinf(ang) * speed, cosf(ang) * speed};
    shots.spawn(transform2_at(at, -ang), s, Velocity{v}, collider_circle(radius, L_SHOT, L_AIR | L_GROUND), shot);
}

} // namespace

const char* weapon_name(u32 weapon) {
    switch (weapon) {
    case WeaponBeam:  return "BEAM";
    case WeaponStorm: return "STORM";
    case WeaponLance: return "LANCE";
    default:          return "SPREAD";
    }
}

void spread_inner(u32 power, bool focus, f32* muzzle_x, f32* angle) {
    const SpreadLevel& l = k_spread[level(power)];
    f32 fan = l.fan * (focus ? kFocusFan : 1.0f);
    // The shots are spread evenly over the fan; the innermost is the middle one, or the
    // one beside the middle.
    u32 i = l.shots / 2;
    f32 f = (f32)i / (f32)(l.shots - 1) - 0.5f;
    *angle = f * fan * kDeg;
    *muzzle_x = f * 2.0f * kMuzzle;
}

u32 weapon_fire(Game& g, Table& shots, point2 at, u32 weapon, u32 power, bool focus) {
    u32 lv = level(power);
    switch (weapon) {
    case WeaponSpread: {
        const SpreadLevel& l = k_spread[lv];
        f32 fan = l.fan * (focus ? kFocusFan : 1.0f) * kDeg;
        Sprite sp = sprite_of(g, TEX_TILES, tile_uv(0, 0), vec2{24, 24}, Z_SHOT);
        for (u32 i = 0; i < l.shots; ++i) {
            f32 f = (f32)i / (f32)(l.shots - 1) - 0.5f;
            shot_spawn(shots, sp, at + vec2{f * 2.0f * kMuzzle, kMuzzleUp}, f * fan, kShotSpeed, 6.0f, Shot{l.damage, 0, ShotPlain, 0.0f});
        }
        return l.every;
    }
    case WeaponStorm: {
        const StormLevel& l = k_storm[lv];
        Sprite sp = sprite_of(g, TEX_TILES, tile_uv(3, 0), vec2{20, 20}, Z_SHOT, color{0.55f, 1.0f, 0.6f, 1.0f});
        for (u32 i = 0; i < l.bolts; ++i) {
            // Out to the sides first, then they find their mark.
            f32 f = l.bolts == 1 ? 0.0f : (f32)i / (f32)(l.bolts - 1) - 0.5f;
            shot_spawn(shots, sp, at + vec2{f * 24.0f, kMuzzleUp}, f * 70.0f * kDeg, kBoltSpeed, 5.0f, Shot{l.damage, 0, ShotBolt, l.turn * kDeg});
        }
        return l.every;
    }
    case WeaponLance: {
        const LanceLevel& l = k_lance[lv];
        Sprite sp = sprite_of(g, TEX_TILES, tile_uv(0, 0), vec2{l.size, l.size * 2.0f}, Z_SHOT, color{1.0f, 0.9f, 0.55f, 1.0f});
        shot_spawn(shots, sp, at + vec2{0.0f, kMuzzleUp + 6.0f}, 0.0f, l.speed, l.size * 0.45f, Shot{l.damage, 0, ShotLance, 0.0f});
        return l.every;
    }
    default: return 6; // the Beam fires itself (`beams`); this is only how often it sounds
    }
}

Handle beam_spawn(Engine& e, Game& g, Handle owner) {
    // Blended, not added: added, it washes out to white over the sea.
    Sprite s = sprite_of(g, TEX_BEAM, rect2_unit(), vec2{0, 0}, Z_SHOT, color{1.0f, 1.0f, 1.0f, 0.95f});
    return engine_table(e, g_beam_kind)->spawn(transform2_at(point2{0, 0}), local2_at(point2{0, kMuzzleUp}), Parent{owner}, s, Beam{0, 0.0f, 0.0f, 0});
}

// Phase Sim, after control: shots fly; a bolt turns toward the nearest enemy (as things stood
// when the tick began: an air or ground row with health, a boss's open part); what leaves
// the field goes.
void shots(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    MemScope scope;
    Array<point2> targets = array_in<point2>(mem_scratch());
    Table* st = ctx.write_kind(g_shot_kind);
    Span<Transform2> t = st->rows_of<Transform2>();
    Span<Velocity> v = st->rows_of<Velocity>();
    Span<const Shot> sh = st->rows_of<Shot>();
    bool bolts = false;
    for (u32 i = 0; i < st->rows() && !bolts; ++i) bolts = st->is_alive(i) && sh[i].kind == ShotBolt;
    if (bolts) {
        const u32 kinds[] = {g_air_kind, g_ground_kind};
        for (u32 kind : kinds) {
            const Table* k = engine_table(e, kind);
            Span<const Transform2> kx = k->rows_of<Transform2>();
            Span<const Health> kh = k->rows_of<Health>();
            for (u32 i = 0; i < k->rows(); ++i)
                if (k->is_alive(i) && kh[i].hp > 0 && in_field(kx[i].position, 0.0f)) targets.push(kx[i].position);
        }
        const Table* parts = engine_table(e, g_part_kind);
        const Table* bosses = engine_table(e, g_boss_kind);
        Span<const Transform2> px = parts->rows_of<Transform2>();
        Span<const Part> pp = parts->rows_of<Part>();
        Span<const Health> ph = parts->rows_of<Health>();
        for (u32 i = 0; i < parts->rows(); ++i) {
            if (!parts->is_alive(i) || ph[i].hp <= 0) continue;
            const Boss* b = bosses->get<Boss>(pp[i].boss);
            if (b && g.bosses[b->def].parts[pp[i].index].armored_until <= b->phase) targets.push(px[i].position);
        }
    }
    for (u32 i = 0; i < st->rows(); ++i) {
        if (!st->is_alive(i)) continue;
        point2& p = t[i].position;
        if (sh[i].kind == ShotBolt && targets.len) {
            const point2* best = nullptr;
            f32 bd = 1e18f;
            for (const point2& q : targets) { f32 d = length_sq(q - p); if (d < bd) { bd = d; best = &q; } }
            // Turn toward it, no faster than the bolt's rate.
            f32 have = atan2f(v[i].v.x, v[i].v.y), want = atan2f(best->x - p.x, best->y - p.y);
            f32 d = want - have;
            while (d > KV_PI) d -= 2.0f * KV_PI;
            while (d < -KV_PI) d += 2.0f * KV_PI;
            f32 turn = sh[i].turn;
            have += d < -turn ? -turn : d > turn ? turn : d;
            f32 speed = length(v[i].v);
            v[i].v = vec2{sinf(have) * speed, cosf(have) * speed};
            t[i].rotation = -have;
        }
        p += v[i].v;
        if (!in_field(p, kCullMargin)) ctx.destroy(st->handle_at(i));
    }
}

// Phase Sim, after the collision module has placed everything this tick: each beam whose
// owner holds the Beam and the shot shows, cut at the first body on the enemies' layers it
// meets (a boss's hull among them), and damages it every few ticks by event (BeamHit, whose
// handler is the shots' hit). Hidden otherwise.
void beams(TickContext& ctx) {
    Engine& e = *ctx.engine;
    // The columns it writes (not the kind: a beam's transform is the compose's, which this
    // follows); the table for liveness and the parents.
    const Table* bt = engine_table(e, g_beam_kind);
    Span<Local2> bl = ctx.write<Local2>("example_arcfire.beam.Local2");
    Span<Sprite> bs = ctx.write<Sprite>("example_arcfire.beam.Sprite");
    Span<Beam> bb = ctx.write<Beam>("example_arcfire.beam.Beam");
    Span<const Parent> bp = ctx.read<Parent>("example_arcfire.beam.Parent");
    const Table* ships = engine_table(e, g_ship_kind);
    MemScope scope;
    Array<CollisionHit> hits = array_in<CollisionHit>(mem_scratch());
    for (u32 i = 0; i < bt->rows(); ++i) {
        if (!bt->is_alive(i)) continue;
        Beam& b = bb[i];
        const Ship* s = ships->get<Ship>(bp[i].parent);
        const Transform2* from = ships->get<Transform2>(bp[i].parent);
        if (!s || !from || !s->live || !s->firing || s->weapon != WeaponBeam) {
            bs[i].size = vec2{0, 0};
            b.length = 0.0f;
            b.timer = 0;
            continue;
        }
        const BeamLevel& l = k_beam[level(s->power)];
        f32 width = l.width * (s->focus ? kBeamFocusWidth : 1.0f);
        point2 o = from->position + vec2{0.0f, kMuzzleUp};
        hits.clear();
        collision_cast_all(ctx, rect2{o.x - width * 0.5f, o.y, width, 1.0f}, vec2{0.0f, kBeamReach}, L_AIR | L_GROUND, hits);
        f32 len = hits.len ? hits[0].t * kBeamReach : kBeamReach;
        b.length = len;
        b.scroll += kBeamScroll;
        if (b.scroll > 1.0f) b.scroll -= 1.0f;
        bl[i].position = point2{0.0f, kMuzzleUp + len * 0.5f};
        bs[i].size = vec2{width + 6.0f, len}; // the glow wider than the body it hurts
        bs[i].uv = rect2{0.0f, -b.scroll, 1.0f, len / kBeamStrip};
        if (hits.len && (b.timer == 0 || --b.timer == 0)) {
            b.timer = l.every;
            i32 dmg = (i32)ceilf((f32)l.damage * (s->focus ? kBeamFocusDamage : 1.0f));
            ctx.emit(BeamHit{hits[0].entity, dmg, 0, o + vec2{0.0f, len}});
        }
    }
}

} // namespace kv::arcfire
