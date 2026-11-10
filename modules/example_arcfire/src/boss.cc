// example_arcfire: bosses. A boss is a hull (`boss`, one row) and its parts (`part`, a row
// each, children of the hull or of another part), from its definition (boss_data.cc). The
// collision module composes the parts where the hull is before it tests them, so a turret
// on a swaying hull is hit where it is drawn.
//
// The life of one: a boss mark spawns it above the field; it comes down its entry path and
// holds, swaying as its phase says. Shots fly over its hull; the hull is a body the ship
// must not touch. Its parts fire while their phase has come (before it they are armored:
// shots spark off them) and its bays launch popcorn. A part's health crossing below one is
// a state trigger (`part_down`); the handler takes the part away with whatever rides on it,
// pays for it, and moves the boss to its next phase once the parts that end this one are
// down. While one is up the stage's marks wait (stage.cc). It ends one of two ways:
//
//   - The core's death is the boss's. Time slows to a quarter for a moment, as at the
//     ship's death; every bullet becomes a chip; the bonus is paid by the time it had
//     left; the parts still standing burst in turn and the hull breaks in pieces.
//   - Its time runs out: it stomps. Every part still standing fires the boss's stomp
//     pattern, its bays launch at once, the screen tints, and the hull comes down the field
//     and back, a body the ship must not touch. It is meant to cost an egg; a bomb is the
//     way out. Then it leaves upward, paying nothing for what stands.
#include "arcfire.h"
#include "korvid/asset/blob.h"
#include "korvid/asset/material.h"
#include "korvid/asset/scope.h"
#include "korvid/asset/texture.h"
#include "korvid/core/log.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace kv::arcfire {

namespace {

// Every boss the game has, loaded at startup in this order: their materials' handles are
// sim state (a sprite's), so they are made at the same point every run.
constinit const char* const k_boss_names[] = {"carrier", "battleship"};

constexpr vec2 kBossShadow{30.0f, -56.0f}; // higher than an air row's
constexpr f32  kBossShadowAlpha = 0.33f;
constexpr f32  kLeaveSpeed = 2.5f;         // units a tick, up and away
constexpr f32  kSwayFollow = 0.06f;        // how fast the hull follows its sway (a phase's change eases in)
constexpr u32  kFirstFire = 50, kFireStagger = 17; // a part's first pattern after the hold, staggered by its index
constexpr u32  kBayOpen = 50;              // ticks a bay shows open around a launch
constexpr u32  kStompRest = 20;            // ticks between a part's stomp patterns
// The kill: a quarter speed, eased in and out (sim ticks: at a quarter, 15 are a second);
// a part bursts every few ticks; the hull breaks once they are gone.
constexpr u32  kKillIn = 4, kKillHold = 15, kKillOut = 12, kBurstEvery = 7, kBreakAt = 24;
constexpr f32  kKillSlow = 0.25f;
const color kSparkPing{0.75f, 0.8f, 0.9f, 1.0f};
const color kSparkPart{1.0f, 0.7f, 0.3f, 1.0f};

Handle boss_material(Engine& e, Handle texture, BlendMode blend) {
    const MaterialAsset* m = asset_material(e, asset_material_for_texture(e, texture, blend, TextureFilter::Nearest, asset_scope_global(e)));
    return m ? m->material : Handle{};
}

// Whether part `index` rides, at any depth, on part `on`.
bool rides_on(const BossDef& def, u32 index, u32 on) {
    for (i32 k = (i32)index; k >= 0; k = def.parts[k].parent)
        if ((u32)k == on) return true;
    return false;
}

f32 kill_scale(u32 t) {
    if (t < kKillIn) return 1.0f - (1.0f - kKillSlow) * (f32)(t + 1) / (f32)kKillIn;
    if (t < kKillIn + kKillHold) return kKillSlow;
    if (t < kKillIn + kKillHold + kKillOut) return kKillSlow + (1.0f - kKillSlow) * (f32)(t - kKillIn - kKillHold + 1) / (f32)kKillOut;
    return 1.0f;
}

// The hull in pieces: a grid of its own sprite, flung out from its middle.
void hull_breaks(TickContext& ctx, Game& g, const BossDef& def, Handle material, point2 at) {
    constexpr u32 kCols = 4, kRows = 4;
    vec2 piece{def.size.x / (f32)kCols, def.size.y / (f32)kRows};
    for (u32 r = 0; r < kRows; ++r)
        for (u32 c = 0; c < kCols; ++c) {
            vec2 off{((f32)c + 0.5f) * piece.x - def.size.x * 0.5f, def.size.y * 0.5f - ((f32)r + 0.5f) * piece.y};
            vec2 v = off * 0.012f + vec2{g.fx.range(-0.6f, 0.6f), g.fx.range(-0.2f, 0.8f)};
            rect2 uv{(f32)c / (f32)kCols, (f32)r / (f32)kRows, 1.0f / (f32)kCols, 1.0f / (f32)kRows};
            fx_piece_of(ctx, material, at + off, uv, piece, v, g.fx.range(-0.05f, 0.05f), 80, Z_BOSS, 0.03f);
            if ((r + c) % 2 == 0) fx_boom(ctx, g, at + off, 72.0f);
        }
}

} // namespace

void bosses_load(Engine& e, Game& g) {
    g.boss_count = KV_COUNTOF(k_boss_names);
    for (u32 i = 0; i < g.boss_count; ++i) {
        char path[128];
        snprintf(path, sizeof path, "example_arcfire/bosses/%s.boss.json", k_boss_names[i]);
        g.boss_blobs[i] = asset_load_blob(e, path);
        for (u32 k = 0; k < 2; ++k) {
            snprintf(path, sizeof path, "example_arcfire/generated/%s%s.png", k_boss_names[i], k ? "_parts" : "");
            g.boss_tex[i][k] = asset_load_texture(e, path);
            g.boss_alpha[i][k] = boss_material(e, g.boss_tex[i][k], BlendMode::Alpha);
            g.boss_additive[i][k] = boss_material(e, g.boss_tex[i][k], BlendMode::Additive);
        }
        // Its hull burning away, the dissolve shader's (a material file, as a game shader's are).
        snprintf(path, sizeof path, "example_arcfire/materials/%s_dissolve.material.json", k_boss_names[i]);
        g.boss_dissolve[i] = asset_load_material(e, path);
    }
}

// The files, once in, read into the definitions (their paths and bays' formations into
// the stage), and the stage's boss marks matched to them. Once.
bool bosses_ready(Engine& e, Game& g, char* why, usize why_cap) {
    if (g.bosses_parsed) return true;
    for (u32 i = 0; i < g.boss_count; ++i) {
        const BlobAsset* b = asset_blob(e, g.boss_blobs[i]);
        if (!b || b->state == AssetState::Pending) return false;
        if (b->state != AssetState::Ready) { snprintf(why, why_cap, "boss %s did not load", k_boss_names[i]); return false; }
    }
    for (u32 i = 0; i < g.boss_count; ++i) {
        const BlobAsset* b = asset_blob(e, g.boss_blobs[i]);
        if (!boss_parse((const char*)b->data, b->size, k_boss_names[i], g.bosses[i], g.stage, why, why_cap)) return false;
    }
    for (u32 m = 0; m < g.stage.boss_mark_count; ++m) {
        g.boss_of[m] = kMaxBosses;
        for (u32 i = 0; i < g.boss_count; ++i)
            if (!strcmp(g.stage.boss_names[m], g.bosses[i].name)) g.boss_of[m] = i;
        if (g.boss_of[m] == kMaxBosses) { snprintf(why, why_cap, "a boss mark names \"%s\", which the game does not have", g.stage.boss_names[m]); return false; }
    }
    g.bosses_parsed = true;
    return true;
}

// The hull above the field at its entry's start, its parts on it (parents before their
// riders, as the file lists them), and its shadow.
Handle boss_spawn(TickContext& ctx, Game& g, const Dials& d, u32 di) {
    const BossDef& b = g.bosses[di];
    Table* boss_table = ctx.write_kind(g_boss_kind);
    Table* parts = ctx.write_kind(g_part_kind);
    point2 at = path_at(b.entry, 0.0f);
    Sprite hull;
    hull.material = g.boss_alpha[di][0];
    hull.size = b.size;
    hull.layer = Z_BOSS;
    Collider2 col = collider_box(b.hull_half, L_AIR, L_SHIP); // a body to the ship; the shots fly over it
    col.offset = b.hull_offset;
    Handle h = boss_table->spawn(transform2_at(at), hull, col, Boss{di, 0, (u32)BossState::Entering, 0, 0.0f, 0, at});
    if (h.is_null()) return h;
    Handle made[kMaxParts];
    for (u32 i = 0; i < b.part_count; ++i) {
        const PartDef& p = b.parts[i];
        point2 world = at;
        for (i32 k = (i32)i; k >= 0; k = b.parts[k].parent) world += vec2{b.parts[k].at.x, b.parts[k].at.y};
        Sprite s;
        s.material = g.boss_alpha[di][1];
        s.uv = p.uv;
        s.size = p.size;
        s.layer = Z_PART;
        Collider2 c = collider_circle(p.radius, L_AIR, L_SHOT);
        Health hp{(i32)ceilf((f32)p.health * d.enemy_health), 0, p.score, 0};
        Emitter em{p.pattern_count ? p.entries[0] : KV_BML_NONE, 0.0f, EmitIdle, kFirstFire + i * kFireStagger};
        Part part{h, i, 0, p.launch != kNoFormation ? p.every / 2 : 0, 0};
        made[i] = parts->spawn(transform2_at(world), local2_at(p.at), Parent{p.parent < 0 ? h : made[p.parent]}, s, c, hp, em, Runner{}, part);
    }
    Sprite shadow = hull;
    shadow.tint = color{0.0f, 0.0f, 0.04f, kBossShadowAlpha};
    shadow.layer = Z_SHADOW;
    ctx.spawn_later(g_shadow_kind, Transform2{}, local2_at(point2{kBossShadow.x, kBossShadow.y}), Parent{h}, shadow);
    sfx(ctx, g, SFX_WARNING);
    music(ctx, g, true);
    KV_INFO("arcfire", "boss: %s at tick %llu, %u parts, %d health, %u s", b.title, (unsigned long long)ctx.tick, b.part_count, b.total, b.timeout / 60);
    return h;
}

bool part_absorbs(const Game& g, TickContext& ctx, Handle part) {
    Span<const Part> pa = ctx.read<Part>("example_arcfire.part.Part");
    Span<const Boss> bo = ctx.read<Boss>("example_arcfire.boss.Boss");
    if (part.index() >= pa.len) return true;
    const Part& p = pa[part.index()];
    if (p.boss.index() >= bo.len) return true;
    const Boss& b = bo[p.boss.index()];
    const PartDef& def = g.bosses[b.def].parts[p.index];
    return def.armored_until > b.phase || b.state == (u32)BossState::Leaving || b.state == (u32)BossState::Dying;
}

// Phase Sim, after the stage (which spawns them) and before the patterns (which run what
// the parts start): the hulls move, stomp, leave or die; the parts open, fire, launch and
// flash. On the main thread: a kill sets the engine's time scale.
void bosses(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    Span<const Run> run_row = ctx.read<Run>(ARC_WORLD(Run));
    Span<const Dials> dials_row = ctx.read<Dials>(ARC_WORLD(Dials));
    Span<const EggState> egg_row = ctx.read<EggState>(ARC_WORLD(EggState));
    if (!run_row.len) return;
    // The egg's slow time is the egg's: a kill takes the clock only while no egg plays.
    EggStep step = egg_row.len ? (EggStep)egg_row[0].step : EggStep::None;
    bool clock_free = step == EggStep::None || step == EggStep::Shelled;
    Table* bt = ctx.write_kind(g_boss_kind);
    Table* pt = ctx.write_kind(g_part_kind);
    Span<Transform2> bx = bt->rows_of<Transform2>();
    Span<Boss> bb = bt->rows_of<Boss>();
    Span<Transform2> px = pt->rows_of<Transform2>();
    Span<Emitter> pe = pt->rows_of<Emitter>();
    Span<Part> pp = pt->rows_of<Part>();
    for (u32 i = 0; i < bt->rows(); ++i) {
        if (!bt->is_alive(i)) continue;
        Handle me = bt->handle_at(i);
        Boss& b = bb[i];
        const BossDef& def = g.bosses[b.def];
        point2& p = bx[i].position;
        ++b.clock;
        switch ((BossState)b.state) {
        case BossState::Entering:
            b.s += def.speed;
            p = path_at(def.entry, b.s);
            if (b.s >= def.entry.length) {
                b.state = (u32)BossState::Holding;
                b.hold = p;
                b.clock = 0;
            }
            break;
        case BossState::Holding: {
            const PhaseDef& ph = def.phases[b.phase < def.phase_count ? b.phase : def.phase_count - 1];
            f32 t = (f32)b.clock;
            point2 want{b.hold.x + sinf(t * ph.sway_rate) * ph.sway, b.hold.y + sinf(t * ph.sway_rate * 1.7f) * 6.0f};
            p += (want - p) * kSwayFollow;
            if (b.clock >= def.timeout) {
                // Out of time: the stomp. Every part still standing turns to the stomp's
                // pattern, a bay launches now.
                b.state = (u32)BossState::Stomping;
                b.clock = 0;
                for (u32 k = 0; k < pt->rows(); ++k) {
                    if (!pt->is_alive(k) || pp[k].boss != me) continue;
                    pe[k].state = EmitIdle;
                    pe[k].pattern = def.stomp_entry;
                    pe[k].timer = 1 + pp[k].index * 5;
                    if (def.parts[pp[k].index].launch != kNoFormation) pp[k].timer = 1;
                }
                sfx(ctx, g, SFX_WARNING, 0.8f);
                KV_INFO("arcfire", "boss: %s is out of time at tick %llu: it stomps", def.title, (unsigned long long)ctx.tick);
            }
            break;
        }
        case BossState::Stomping: {
            f32 t = (f32)b.clock / (f32)(def.stomp_ticks ? def.stomp_ticks : 1);
            p.y = b.hold.y - def.dive * sinf(KV_PI * (t < 1.0f ? t : 1.0f));
            p.x += (b.hold.x - p.x) * kSwayFollow;
            if (b.clock >= def.stomp_ticks) {
                b.state = (u32)BossState::Leaving;
                KV_INFO("arcfire", "boss: %s leaves at tick %llu", def.title, (unsigned long long)ctx.tick);
            }
            break;
        }
        case BossState::Leaving:
            p.y += kLeaveSpeed;
            if (p.y - def.size.y * 0.5f > kFieldTop + 40.0f) {
                for (u32 k = 0; k < pt->rows(); ++k)
                    if (pt->is_alive(k) && pp[k].boss == me) ctx.destroy(pt->handle_at(k));
                ctx.destroy(me);
                KV_INFO("arcfire", "boss: %s gone at tick %llu", def.title, (unsigned long long)ctx.tick);
            }
            break;
        case BossState::Dying: {
            if (clock_free) engine_time_scale(e, kill_scale(b.clock));
            // The parts still standing burst in turn, lowest index first; then the hull.
            u32 left = 0, first = ~0u;
            for (u32 k = 0; k < pt->rows(); ++k)
                if (pt->is_alive(k) && pp[k].boss == me) { ++left; if (first == ~0u || pp[k].index < pp[first].index) first = k; }
            // The hull burns away (the dissolve shader, its progress in the tint's alpha) as it
            // dies: the time so far over that and what is left (a burst a part, the break's
            // least), so it is nine tenths gone as it breaks, however many parts it had.
            if (Sprite* hull = bt->get<Sprite>(me)) {
                if (const MaterialAsset* burn = asset_material(e, g.boss_dissolve[b.def])) hull->material = burn->material;
                u32 settle = kKillIn + kKillHold + kKillOut;
                f32 rest = (f32)(kBurstEvery * left) + (f32)(b.clock < settle ? settle - b.clock : 0u);
                hull->tint.a = 0.9f * (f32)b.clock / ((f32)b.clock + rest + 1.0f);
                // Its shadow (a cosmetic child) fades as it goes.
                Table* st = ctx.write_kind(g_shadow_kind);
                Span<const Parent> sp = st->rows_of<Parent>();
                Span<Sprite> ss = st->rows_of<Sprite>();
                for (u32 k = 0; k < st->rows(); ++k)
                    if (st->is_alive(k) && sp[k].parent == me) ss[k].tint.a = kBossShadowAlpha * (1.0f - hull->tint.a / 0.9f);
            }
            if (left && b.clock % kBurstEvery == 0) {
                fx_boom(ctx, g, px[first].position, 64.0f);
                fx_sparks(ctx, g, px[first].position, dials_row[0].particles * 2, kSparkPart, 5.0f);
                fx_shimmer(g, px[first].position, 110.0f, 0.7f, 32);
                ctx.destroy(pt->handle_at(first));
                g.shake = g.shake > 6.0f ? g.shake : 6.0f;
            } else if (!left && b.clock >= kBreakAt && b.clock >= kKillIn + kKillHold + kKillOut) {
                hull_breaks(ctx, g, def, g.boss_alpha[b.def][0], p);
                sfx(ctx, g, SFX_RUMBLE);
                fx_sparks(ctx, g, p, dials_row[0].particles * 6, kSparkPart, 7.0f);
                fx_shimmer(g, p, 340.0f, 1.0f, 56);
                g.shake = 14.0f;
                ctx.destroy(me);
                if (clock_free) engine_time_scale(e, 1.0f);
                KV_INFO("arcfire", "boss: %s breaks up at tick %llu", def.title, (unsigned long long)ctx.tick);
            }
            break;
        }
        }
    }

    Span<Sprite> ps = pt->rows_of<Sprite>();
    Span<Health> ph = pt->rows_of<Health>();
    Span<Runner> pr = pt->rows_of<Runner>();
    Table* formations = ctx.write_kind(g_formation_kind);
    for (u32 i = 0; i < pt->rows(); ++i) {
        if (!pt->is_alive(i)) continue;
        Part& part = pp[i];
        const Boss* b = bt->get<Boss>(part.boss);
        if (!b) continue; // its boss is gone: scene.compose takes it
        const BossDef& def = g.bosses[b->def];
        const PartDef& pd = def.parts[part.index];
        Emitter& em = pe[i];
        if (b->state == (u32)BossState::Dying) { em.state = EmitIdle; continue; }
        bool stomp = b->state == (u32)BossState::Stomping;
        bool armored = pd.armored_until > b->phase;
        bool active = stomp || (!armored && b->state == (u32)BossState::Holding);
        // A bay shows open around its launches; anything else once its phase has come, or
        // in the stomp.
        bool open = pd.launch != kNoFormation ? active && (stomp || part.timer + kBayOpen > pd.every) : !armored || stomp;
        ps[i].uv = open ? pd.uv_open : pd.uv;
        if (ph[i].flash) { --ph[i].flash; ps[i].material = g.boss_additive[b->def][1]; }
        else ps[i].material = g.boss_alpha[b->def][1];

        u32 next = stomp ? def.stomp_entry : pd.pattern_count ? pd.entries[part.cycle] : KV_BML_NONE;
        if (!active || next == KV_BML_NONE) {
            if (em.state == EmitRunning) em.state = EmitIdle;
        } else if (em.state == EmitDone) {
            if (!stomp) {
                part.cycle = (part.cycle + 1) % pd.pattern_count;
                next = pd.entries[part.cycle];
            }
            em.pattern = next;
            em.state = EmitIdle;
            em.timer = stomp ? kStompRest : pd.rest;
        } else if (em.state == EmitIdle && (em.timer == 0 || --em.timer == 0)) {
            em.pattern = next;
            emitter_start(g, em, pr[i], pt->handle_at(i), ctx.tick);
        }

        if (pd.launch != kNoFormation && active && part.timer && --part.timer == 0) {
            // A launch: a formation up out of the bay, its path from where the bay is. In
            // the stomp, once.
            part.timer = stomp ? 0 : pd.every;
            const FormationDef& f = g.stage.formations[pd.launch];
            u32 count = (u32)lroundf((f32)f.count * dials_row[0].formation_count);
            if (count == 0) count = 1;
            Handle group = formations->spawn(Formation{count, 0, 0, 0});
            for (u32 m = 0; m < count; ++m)
                ctx.schedule(SpawnMember{pd.launch, m, run_row[0].epoch, 0, group, px[i].position}, ctx.tick + (u64)m * f.spacing);
        }
    }
}

// A part's health crossed below one (the trigger, delivered the tick after): it goes, with
// what rides on it, and pays; its boss moves on a phase if this was the last of the phase's
// parts. The core's death is the boss's: the bullets to chips, the bonus, and the boss
// dies (in `bosses`, over the next ticks).
void on_part_down(TickContext& ctx, Span<const StateEdge> edges) {
    Game& g = ctx.state_as<Game>();
    // The tables for liveness and handles; the columns through the declared sets. The rows
    // go by command.
    const Table* pt = engine_table(*ctx.engine, g_part_kind);
    const Table* bt = engine_table(*ctx.engine, g_boss_kind);
    Span<const Part> parts = ctx.read<Part>("example_arcfire.part.Part");
    Span<const Transform2> part_xf = ctx.read<Transform2>("example_arcfire.part.Transform2");
    Span<Boss> boss_rows = ctx.write<Boss>("example_arcfire.boss.Boss");
    Span<const Dials> dials_row = ctx.read<Dials>(ARC_WORLD(Dials));
    u32 sparks = dials_row.len ? dials_row[0].particles : 8;
    for (const StateEdge& se : edges) {
        if (se.trigger != g_part_down || !pt->valid(se.entity)) continue;
        const Part& down = parts[se.entity.index()];
        Handle boss = down.boss;
        u32 index = down.index;
        if (!bt->valid(boss)) continue;
        Boss* b = &boss_rows[boss.index()];
        if ((b->down & (1u << index)) || b->state == (u32)BossState::Dying) continue;
        const BossDef& def = g.bosses[b->def];
        point2 core_at{0, 0};
        for (u32 i = 0; i < pt->rows(); ++i) {
            if (!pt->is_alive(i)) continue;
            const Part& p = parts[i];
            if (p.boss != boss || !rides_on(def, p.index, index)) continue;
            point2 at = part_xf[i].position;
            if (p.index == index) core_at = at;
            b->down |= 1u << p.index;
            ctx.destroy(pt->handle_at(i));
            ctx.emit(Kill{at, def.parts[p.index].score, 0, Handle{}});
            drop_item(ctx, g, def.parts[p.index].drop, at);
            fx_sparks(ctx, g, at, sparks * 2, kSparkPart, 5.0f); // its Kill brings the explosion (on_kill)
        }
        if (def.parts[index].core) {
            f32 left = b->state == (u32)BossState::Entering ? 1.0f
                     : b->state == (u32)BossState::Holding ? 1.0f - (f32)b->clock / (f32)(def.timeout ? def.timeout : 1) : 0.0f;
            i32 bonus = (i32)((f32)def.score * (left > 0.0f ? left : 0.0f));
            u32 overflow = 0;
            u32 cancelled = cancel_bullets(ctx, g, core_at, 0.0f, &overflow);
            ctx.emit(Kill{core_at, bonus + (i32)overflow * kChipValue, 1, Handle{}}); // the chips' cap's overflow paid at once, as a bomb's
            fx_blast(ctx, g, core_at, Blast::Large);
            sfx(ctx, g, SFX_BOSS_DOWN);
            KV_INFO("arcfire", "boss: %s destroyed at tick %llu, %u s into the fight, bonus %d, %u bullets to chips", def.title,
                    (unsigned long long)ctx.tick, b->clock / 60, bonus, cancelled);
            b->state = (u32)BossState::Dying;
            b->clock = 0;
            continue;
        }
        while (b->phase + 1 < def.phase_count && (b->down & def.phases[b->phase].ends) == def.phases[b->phase].ends) {
            ++b->phase;
            KV_INFO("arcfire", "boss: %s, phase %s at tick %llu", def.title, def.phases[b->phase].name, (unsigned long long)ctx.tick);
        }
    }
}

// A shot that struck an armored part: a spark off it, nothing more.
void boss_ping(TickContext& ctx, Game& g, point2 at) { fx_sparks(ctx, g, at, 1, kSparkPing, 1.5f); }

} // namespace kv::arcfire
