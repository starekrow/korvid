// example_arcfire: the weapons. Four types, each at five power levels, as rows of the
// tables below (the slice 4 PRD's starting numbers; the closing tuning pass settles them):
//
//   Spread  a fan of shots from the nose, more and wider with power, narrowed when focused
//   Beam    continuous, locked to the ship, and shaped: one beam, then two from the ship,
//           then each splitting halfway up, then three, the outer ones split, then all
//           three split. Each segment is cut at the first body it meets (a boss's hull it
//           passes over) and damages it every few ticks: wide coverage, light on any one
//           target. Focused, the shape draws in to a third of its spread.
//           An option's is one beam, straight ahead, leaning as the ship moves sideways
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
constexpr f32 kBeamFocusSpread = 0.35f;
constexpr f32 kBeamSplit = 330.0f;   // where a beam splits: about halfway up the field from the ship

struct SpreadLevel { u32 shots; f32 fan; u32 every; i32 damage; };
constinit const SpreadLevel k_spread[kMaxPower] = {{2, 6, 6, 1}, {3, 10, 6, 1}, {4, 14, 5, 1}, {5, 18, 5, 1}, {7, 24, 5, 1}};
// A level's beam: each segment's width and damage every so many ticks, and its shape: the
// segments, each from the muzzle (parent -1) or the end of another, at an angle (degrees
// clockwise) from its parent's, long to the split or to the field's top.
struct BeamSeg { i32 parent; f32 angle; bool splits; };
struct BeamLevel { f32 width; i32 damage; u32 every; u32 count; BeamSeg segs[kBeamSegments]; };
constinit const BeamLevel k_beam[kMaxPower] = {
    {8, 1, 4, 1, {{-1, 0, false}}},
    {8, 1, 4, 2, {{-1, -10, false}, {-1, 10, false}}},
    {8, 1, 4, 6, {{-1, -10, true}, {-1, 10, true}, {0, -8, false}, {0, 8, false}, {1, -8, false}, {1, 8, false}}},
    {10, 1, 3, 7, {{-1, -14, true}, {-1, 0, false}, {-1, 14, true}, {0, -8, false}, {0, 8, false}, {2, -8, false}, {2, 8, false}}},
    {12, 1, 3, 9, {{-1, -14, true}, {-1, 0, true}, {-1, 14, true}, {0, -8, false}, {0, 8, false}, {1, -7, false}, {1, 7, false}, {2, -8, false}, {2, 8, false}}},
};
// An option's beam: one, straight ahead (leaning after the ship's sideways movement), a
// little wider and harder with the ship's level.
constinit const BeamLevel k_option_beam[kMaxPower] = {
    {6, 1, 5, 1, {{-1, 0, false}}}, {7, 1, 5, 1, {{-1, 0, false}}}, {8, 1, 4, 1, {{-1, 0, false}}},
    {9, 1, 4, 1, {{-1, 0, false}}}, {10, 1, 3, 1, {{-1, 0, false}}},
};
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

void beam_spawn(Engine& e, Game& g, Handle owner) {
    // Blended, not added: added, it washes out to white over the sea.
    Sprite s = sprite_of(g, TEX_BEAM, rect2_unit(), vec2{0, 0}, Z_SHOT, color{1.0f, 1.0f, 1.0f, 0.95f});
    Table* beams_table = engine_table(e, g_beam_kind);
    for (u32 k = 0; k < kBeamSegments; ++k)
        beams_table->spawn(transform2_at(point2{0, 0}), local2_at(point2{0, kMuzzleUp}), Parent{owner}, s, Beam{0, 0.0f, 0.0f, k});
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
// owner holds the Beam and the shot shows, segment by segment in its level's shape (a
// segment after its parent, which comes first in the table), each cut at the first body on
// the enemies' layers it meets but a boss's hull, which it passes over, and damaging it every
// few ticks by event (BeamHit, whose handler is the shots' hit). A split exists only if its
// parent reached it. Hidden otherwise.
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
    const Table* options = engine_table(e, g_option_kind);
    const u16 hull_kind = engine_table(e, g_boss_kind)->kind;
    Span<const Parent> op = ctx.read<Parent>("example_arcfire.option.Parent");
    Span<const Transform2> ox = ctx.read<Transform2>("example_arcfire.option.Transform2");
    Span<const Option> oo = ctx.read<Option>("example_arcfire.option.Option");
    MemScope scope;
    Array<CollisionHit> hits = array_in<CollisionHit>(mem_scratch());
    // Each owner's rows by segment, so a segment is cast after its parent, whatever slots
    // the rows have.
    struct Owner { Handle owner; u32 row[kBeamSegments]; point2 end[kBeamSegments]; f32 angle[kBeamSegments]; bool full[kBeamSegments]; };
    Owner owners[4];
    u32 owner_count = 0;
    for (u32 i = 0; i < bt->rows(); ++i) {
        if (!bt->is_alive(i) || bb[i].seg >= kBeamSegments) continue;
        Handle owner = bp[i].parent;
        Owner* w = nullptr;
        for (u32 k = 0; k < owner_count && !w; ++k) if (owners[k].owner == owner) w = &owners[k];
        if (!w && owner_count < 4) {
            w = &owners[owner_count++];
            w->owner = owner;
            for (u32 k = 0; k < kBeamSegments; ++k) { w->row[k] = ~0u; w->full[k] = false; w->angle[k] = 0.0f; w->end[k] = point2{0, 0}; }
        }
        if (w) w->row[bb[i].seg] = i;
    }
    for (u32 n = 0; n < owner_count; ++n) {
        Owner& w = owners[n];
        // The owner: the ship, or an option of the ship's (which fires two levels lower).
        bool on_option = options->valid(w.owner);
        const Ship* s = ships->get<Ship>(on_option ? op[w.owner.index()].parent : w.owner);
        const Transform2* from = on_option ? &ox[w.owner.index()] : ships->get<Transform2>(w.owner);
        bool firing = s && from && s->live && s->firing && s->weapon == WeaponBeam;
        const BeamLevel& l = (on_option ? k_option_beam : k_beam)[level(s ? s->power : 1)];
        for (u32 seg = 0; seg < kBeamSegments; ++seg) {
            u32 i = w.row[seg];
            if (i == ~0u) continue;
            Beam& b = bb[i];
            const BeamSeg* sg = firing && seg < l.count ? &l.segs[seg] : nullptr;
            if (sg && sg->parent >= 0 && !w.full[sg->parent]) sg = nullptr; // its parent was cut short of the split
            if (!sg) {
                bs[i].size = vec2{0, 0};
                b.length = 0.0f;
                b.timer = 0;
                continue;
            }
            point2 o = sg->parent < 0 ? from->position + vec2{0.0f, kMuzzleUp} : w.end[sg->parent];
            // An option's beams lean after the ship's sideways movement (options.cc).
            f32 lean = on_option && sg->parent < 0 ? oo[w.owner.index()].tilt : 0.0f;
            f32 ang = (sg->parent < 0 ? lean : w.angle[sg->parent]) + sg->angle * (s->focus ? kBeamFocusSpread : 1.0f) * kDeg;
            f32 most = sg->splits ? kBeamSplit : kBeamReach - (sg->parent < 0 ? 0.0f : kBeamSplit);
            vec2 dir{sinf(ang), cosf(ang)};
            f32 width = l.width;
            hits.clear();
            collision_cast_all(ctx, rect2{o.x - width * 0.5f, o.y - width * 0.5f, width, width}, dir * most, L_AIR | L_GROUND, hits);
            const CollisionHit* first = nullptr;
            for (const CollisionHit& h : hits) if (h.entity.kind() != hull_kind) { first = &h; break; } // over a hull
            f32 len = first ? first->t * most : most;
            w.end[seg] = o + dir * len;
            w.angle[seg] = ang;
            w.full[seg] = !first;
            b.length = len;
            b.scroll += kBeamScroll;
            if (b.scroll > 1.0f) b.scroll -= 1.0f;
            // Placed on the owner: its middle, turned along it.
            point2 mid = o + dir * (len * 0.5f);
            bl[i].position = point2{mid.x - from->position.x, mid.y - from->position.y};
            bl[i].rotation = -ang;
            bs[i].size = vec2{width + 6.0f, len}; // the glow wider than the body it hurts
            bs[i].uv = rect2{0.0f, -b.scroll, 1.0f, len / kBeamStrip};
            if (first && (b.timer == 0 || --b.timer == 0)) {
                b.timer = l.every;
                ctx.emit(BeamHit{first->entity, l.damage, 0, o + dir * len});
            }
        }
    }
}

} // namespace kv::arcfire
