// arcfire: the battle over Orbit, half an orbit away. A capital ship (cyan, the
// defenders') drifts down the stage in four sections; the attackers (orange) come at it in
// waves from the field's edges and swarm it, strafing pass after pass, firing into its
// sections, then break off and leave; its fighters (cyan) dogfight among them, each chasing
// an attacker, or with none to chase wheeling after a wandering mote around the ship. A
// tracer that ends is a flash, now and then a burst. The sections break away one at a time
// as the stage goes on, burning away under the dissolve shader with bursts and a ring of
// bent air; with none left the battle is over and every ship flies off the field.
//
// All of it is cosmetic: three ktables the hash never sees, moved by the module's own random
// stream. Every ship steers (a seek with a turn it cannot tighten past its acceleration) and
// faces where it goes. The `fleet` dial is how many ships (300; 3,000 under --stress): every
// one moves every tick, a stress of the extract, the sort and the GPU with nothing of the
// sim in it. The capital ship's state is the stage's clock (each section's break is a
// distance), so a restart puts it back whole.
#include "arcfire.h"
#include "korvid/asset_render/material.h"
#include "korvid/core/log.h"
#include "korvid/math/scalar.h"

namespace kv::arcfire {

namespace {

constexpr f32 kFleetSize = 16.0f;     // a ship, world units: far off, smaller than anything near
constexpr f32 kTracerSpeed = 5.0f;
constexpr u32 kSections = 4;
constexpr vec2 kSectionSize{84.0f, 60.0f}; // a quarter of the ship (56 x 40 pixels) at 1.5x
constexpr f32 kBreakFrom = 0.2f, kBreakStep = 0.18f; // the stage's fractions at which sections break, bow first
constexpr f32 kBreakSpan = 420.0f;    // units of the stage's distance a section takes to burn away
constexpr f32 kDefenders = 0.35f;     // the dial's share that is the capital ship's fighters
constexpr u32 kWaveEvery = 150;       // ticks between the attackers' waves
// The ships' sides and what they are doing.
enum : u32 { SideDefend = 0, SideAttack = 1 };
enum : u32 { Inbound, Strafing, Leaving, Chasing };
// The sides' tints, faint: the battle is scenery, and nothing in it may read as a threat.
const color kSide[2] = {{0.5f, 0.75f, 1.0f, 0.55f}, {1.0f, 0.6f, 0.4f, 0.55f}};
const color kTrace[2] = {{0.6f, 0.85f, 1.0f, 0.8f}, {1.0f, 0.7f, 0.45f, 0.8f}};
const color kHaze{0.5f, 0.52f, 0.66f, 1.0f}; // the capital ship, far off

// Where the capital ship is at a point of the stage: its middle drifting down the field over
// the whole stage, from above the top to near the bottom.
point2 capital_at(const StageDef& st, f32 clock) {
    f32 t = st.length > 0.0f ? clock / st.length : 0.0f;
    if (t > 1.0f) t = 1.0f;
    return point2{-40.0f, kFieldTop + 40.0f - t * (kFieldH + 20.0f)};
}

// A section's break: 0 whole, 1 gone.
f32 section_gone(const StageDef& st, u32 section, f32 clock) {
    f32 at = st.length * (kBreakFrom + kBreakStep * (f32)section);
    f32 t = (clock - at) / kBreakSpan;
    return t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
}

vec2 unit(vec2 v) {
    f32 l = kv::sqrt(v.x * v.x + v.y * v.y);
    return l > 1e-4f ? vec2{v.x / l, v.y / l} : vec2{0.0f, 1.0f};
}

f32 apart(point2 a, point2 b) {
    vec2 d = b - a;
    return kv::sqrt(d.x * d.x + d.y * d.y);
}

// Turn toward `to` no harder than `accel` a tick, no faster than `top`.
void seek(vec2& v, point2 from, point2 to, f32 accel, f32 top) {
    v += unit(to - from) * accel;
    f32 l = kv::sqrt(v.x * v.x + v.y * v.y);
    if (l > top) v = v * (top / l);
}

// Somewhere past the field's side or top, for a wave to come from or go to.
point2 off_field(Game& g) {
    switch (g.fx.below(3)) {
    case 0:  return point2{kFieldLeft - 40.0f, g.fx.range(0.0f, kFieldTop)};
    case 1:  return point2{kFieldRight + 40.0f, g.fx.range(0.0f, kFieldTop)};
    default: return point2{g.fx.range(kFieldLeft, kFieldRight), kFieldTop + 40.0f};
    }
}

void ship_spawn(TickContext& ctx, Game& g, u32 side, point2 at, vec2 v, point2 aim) {
    Sprite s = sprite_of(g, TEX_SHIPS, ship_uv(side ? 2 : 1, side ? 1 : 0), vec2{kFleetSize, kFleetSize}, Z_FLEET, kSide[side]);
    ctx.spawn_later(g_fleet_ktable, transform2_at(at, kv::atan2(v.y, v.x) - 0.5f * KV_PI), s, Velocity{v},
                    Fleet{side, Inbound, 20u + g.fx.below(120), 0u, g.fx.range(0.0f, 6.28f), aim});
}

void tracer(TickContext& ctx, Game& g, u32 side, point2 at, vec2 dir, vec2 carry) {
    vec2 v = dir * kTracerSpeed + carry * 0.3f;
    Sprite s = sprite_of(g, TEX_SOFT, rect2_unit(), vec2{10.0f, 2.5f}, Z_TRACER, kTrace[side], true);
    ctx.spawn_later(g_tracer_ktable, transform2_at(at + dir * 8.0f, kv::atan2(v.y, v.x)), s, Velocity{v}, Tracer{side, 18u + g.fx.below(16)});
}

} // namespace

void battle_reset(Engine& e, Game& g) {
    const u32 ktables[] = {g_fleet_ktable, g_tracer_ktable, g_capital_ktable};
    for (u32 k : ktables) engine_ktable(e, k)->destroy_all();
    if (!g.stage.battle) return;
    KTable* caps = engine_ktable(e, g_capital_ktable);
    point2 mid = capital_at(g.stage, 0.0f);
    for (u32 i = 0; i < kSections; ++i) {
        Sprite s = sprite_of(g, TEX_CAPITAL, rect2{(f32)i / (f32)kSections, 0.0f, 1.0f / (f32)kSections, 1.0f}, kSectionSize, Z_CAPITAL, kHaze);
        point2 at{mid.x + ((f32)i - 1.5f) * kSectionSize.x, mid.y};
        caps->spawn(transform2_at(at), s, Capital{i, 0});
    }
}

// Phase Sim, unpaused: the capital ship placed and broken by the stage's clock; the fleet
// to the dial's count, steering, firing; the tracers flown.
void battle(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Span<const Stage> stage_row = ctx.read<Stage>(ARC_WORLD(Stage));
    Span<const Dials> dials_row = ctx.read<Dials>(ARC_WORLD(Dials));
    if (!g.stage.battle || !stage_row.len || !dials_row.len) return;
    const Stage& st = stage_row[0];

    // The capital ship: where the stage's clock puts it, each section whole, burning or gone.
    // A section's break begins with a ring of bent air; bursts go off along it while it burns.
    KTable* ct = ctx.write_ktable(g_capital_ktable);
    Span<Transform2> cx = ct->rows_of<Transform2>();
    Span<Sprite> cs = ct->rows_of<Sprite>();
    Span<const Capital> cc = ct->rows_of<Capital>();
    f32 clock = stage_clock(st);
    point2 mid = capital_at(g.stage, clock);
    const MaterialAsset* burn = asset_material(*ctx.engine, g.capital_dissolve);
    point2 standing[kSections]; // the sections still there: the attackers' targets
    u32 targets = 0;
    for (u32 i = 0; i < ct->rows(); ++i) {
        if (!ct->is_alive(i)) continue;
        u32 k = cc[i].section;
        point2 at{mid.x + ((f32)k - 1.5f) * kSectionSize.x, mid.y};
        bool jumped = at.y > cx[i].position.y + 1.0f; // the clock went back: a restart
        cx[i].position = at;
        if (jumped) *ct->prev<Transform2>(ct->handle_at(i)) = cx[i];
        f32 gone = section_gone(g.stage, k, clock);
        f32 before = section_gone(g.stage, k, clock - st.speed);
        if (gone < 0.5f && targets < kSections) standing[targets++] = at;
        if (gone <= 0.0f) {
            cs[i].material = g.alpha[TEX_CAPITAL];
            cs[i].tint = kHaze;
            continue;
        }
        if (burn) cs[i].material = burn->material;
        cs[i].tint = color{kHaze.r, kHaze.g, kHaze.b, gone};
        if (before <= 0.0f) {
            fx_shimmer(g, at, 160.0f, 0.8f, 50);
            KV_INFO("arcfire", "battle: the capital ship's section %u breaks away at tick %llu", k + 1, (unsigned long long)ctx.tick);
        }
        if (gone < 1.0f && g.fx.below(10) == 0)
            fx_boom(ctx, g, at + vec2{g.fx.range(-0.45f, 0.45f) * kSectionSize.x, g.fx.range(-0.35f, 0.35f) * kSectionSize.y}, 30.0f, Z_TRACER);
    }

    // The fleet.
    u32 want = dials_row[0].fleet;
    u32 want_defend = (u32)((f32)want * kDefenders), want_attack = want - want_defend;
    KTable* ft = ctx.write_ktable(g_fleet_ktable);
    Span<Transform2> fx = ft->rows_of<Transform2>();
    Span<Velocity> fv = ft->rows_of<Velocity>();
    Span<Fleet> fl = ft->rows_of<Fleet>();
    u32 count[2] = {0, 0};
    for (u32 i = 0; i < ft->rows(); ++i) {
        if (!ft->is_alive(i)) continue;
        Fleet& f = fl[i];
        u32 side = f.side; // a destroyed row is not read again
        if (++count[side] > (side == SideDefend ? want_defend : want_attack)) { ft->destroy(ft->handle_at(i)); --count[side]; continue; }
        point2& p = fx[i].position;
        vec2& v = fv[i].v;
        if (f.cooldown) --f.cooldown;
        if (f.side == SideAttack) {
            // A pass at a section: in at a point on it, through and out past it, round again;
            // after a few, or with nothing left to strafe, home past the field's edge.
            if (!targets && f.state != Leaving) { f.state = Leaving; f.aim = off_field(g); }
            if (f.state == Inbound) {
                point2 target = standing[f.passes % targets] + vec2{f.aim.x, f.aim.y};
                seek(v, p, target, 0.16f, 3.0f);
                if (apart(p, target) < 40.0f) {
                    if (!f.cooldown) { tracer(ctx, g, f.side, p, unit(target - p), v); f.cooldown = 12; }
                    f.state = Strafing;
                    f.aim = p + unit(v) * g.fx.range(90.0f, 160.0f) + vec2{g.fx.range(-50.0f, 50.0f), g.fx.range(-50.0f, 50.0f)};
                }
            } else if (f.state == Strafing) {
                seek(v, p, f.aim, 0.16f, 3.0f);
                if (apart(p, f.aim) < 30.0f) {
                    ++f.passes;
                    if (f.passes >= 2 + (u32)(f.phase * 0.5f)) { f.state = Leaving; f.aim = off_field(g); }
                    else { f.state = Inbound; f.aim = point2{g.fx.range(-30.0f, 30.0f), g.fx.range(-20.0f, 20.0f)}; }
                }
            } else {
                seek(v, p, f.aim, 0.12f, 3.4f);
                if (!in_field(p, 30.0f)) { ft->destroy(ft->handle_at(i)); --count[side]; continue; }
            }
        } else if (!targets) {
            // The capital ship is gone: its fighters go too, off the field, and do not come back.
            if (f.state != Leaving) { f.state = Leaving; f.aim = off_field(g); }
            seek(v, p, f.aim, 0.14f, 3.4f);
            if (!in_field(p, 30.0f)) { ft->destroy(ft->handle_at(i)); --count[side]; continue; }
        } else {
            // A fighter: after the attacker it chases while that one is still about, else
            // after a mote wheeling round the capital ship (and from it, a new attacker to
            // chase now and then); a tracer when its quarry is close and dead ahead.
            point2 quarry;
            bool chasing = false;
            if (f.state == Chasing && f.passes < ft->rows() && ft->is_alive(f.passes) && fl[f.passes].side == SideAttack) {
                quarry = fx[f.passes].position + fv[f.passes].v * 6.0f; // where it will be
                chasing = in_field(quarry, 0.0f);
            }
            if (!chasing) {
                f.state = Inbound;
                f.phase += 0.025f;
                point2 centre = targets ? standing[0] : mid;
                quarry = centre + vec2{kv::cos(f.phase) * (60.0f + 40.0f * kv::sin(f.phase * 0.37f)), kv::sin(f.phase * 1.3f) * 70.0f};
                u32 k = g.fx.below(ft->rows() ? ft->rows() : 1);
                if (g.fx.below(40) == 0 && ft->is_alive(k) && fl[k].side == SideAttack) { f.state = Chasing; f.passes = k; }
            }
            seek(v, p, quarry, 0.2f, 3.4f);
            vec2 to = quarry - p;
            f32 ahead = to.x * v.x + to.y * v.y, reach = kv::sqrt(to.x * to.x + to.y * to.y) * kv::sqrt(v.x * v.x + v.y * v.y);
            if (chasing && !f.cooldown && apart(p, quarry) < 120.0f && ahead > 0.9f * reach) {
                tracer(ctx, g, f.side, p, unit(v), v);
                f.cooldown = 20 + g.fx.below(30);
            }
        }
        p += v;
        fx[i].rotation = kv::atan2(v.y, v.x) - 0.5f * KV_PI;
    }
    // Reinforcements while the capital ship stands: the fighters, from round it; the
    // attackers, a wave at a time from one edge, in a loose group.
    for (u32 n = 0; targets && count[SideDefend] < want_defend && n < 32; ++n, ++count[SideDefend]) {
        point2 at = mid + vec2{g.fx.range(-160.0f, 160.0f), g.fx.range(-60.0f, 60.0f)};
        ship_spawn(ctx, g, SideDefend, at, vec2{g.fx.range(-2.0f, 2.0f), g.fx.range(-2.0f, 2.0f)}, at);
    }
    if (targets && ctx.tick % kWaveEvery == 0 && count[SideAttack] < want_attack) {
        u32 wave = want_attack / 6 > 6 ? want_attack / 6 : 6;
        if (wave > want_attack - count[SideAttack]) wave = want_attack - count[SideAttack];
        point2 from = off_field(g);
        vec2 in = unit(standing[0] - from) * 2.5f;
        for (u32 n = 0; n < wave; ++n) {
            point2 at = from + vec2{g.fx.range(-40.0f, 40.0f), g.fx.range(-40.0f, 40.0f)};
            ship_spawn(ctx, g, SideAttack, at, in, point2{g.fx.range(-30.0f, 30.0f), g.fx.range(-20.0f, 20.0f)});
        }
    }

    // The tracers: straight on, then a flash where each ends (a hit, or a miss in the dark),
    // and one in twenty-five a burst.
    KTable* tt = ctx.write_ktable(g_tracer_ktable);
    Span<Transform2> tx = tt->rows_of<Transform2>();
    Span<const Velocity> tv = tt->rows_of<Velocity>();
    Span<Tracer> tr = tt->rows_of<Tracer>();
    for (u32 i = 0; i < tt->rows(); ++i) {
        if (!tt->is_alive(i)) continue;
        tx[i].position += tv[i].v;
        if (--tr[i].life) continue;
        point2 at = tx[i].position;
        u32 side = tr[i].side;
        tt->destroy(tt->handle_at(i));
        if (g.fx.below(3) == 0) {
            Sprite s = sprite_of(g, TEX_SPARK, rect2_unit(), vec2{9.0f, 9.0f}, Z_TRACER, kTrace[1 - side], true);
            ctx.spawn_later(g_particle_ktable, transform2_at(at), s, Velocity{vec2{0.0f, 0.0f}}, Particle{10, 10, 0.0f, 1.0f, 1.0f, 0.0f});
        }
        if (g.fx.below(25) == 0) fx_boom(ctx, g, at, 22.0f, Z_TRACER);
    }
}

} // namespace kv::arcfire
