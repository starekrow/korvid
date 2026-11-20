// arcfire: hits. The collision module reports overlaps between colliders whose
// masks meet; `on_overlap` sorts each pair by its ktables into what it means (a shot on an
// enemy, a bullet on the hitbox, an enemy on the hitbox, a bullet through the graze ring,
// a bullet on the shell), and a handler each does the rest. They write the run in a chain
// (module.cc), so a bullet that hits the hitbox takes back its own graze before the
// grazes are counted. A boss's armored parts take shots without harm; its open parts take
// the damage and die by their trigger (boss.cc), not here. Shots fly over a boss's hull.
#include "arcfire.h"
#include "korvid/core/log.h"

namespace kv::arcfire {

namespace {

constexpr i64 kGrazeScore = 10;
constexpr u32 kFlashTicks = 2;
const color kSparkHit{1.0f, 0.85f, 0.5f, 1.0f};
const color kSparkGraze{0.6f, 0.9f, 1.0f, 1.0f};
const color kSparkKill{1.0f, 0.6f, 0.25f, 1.0f};

u16 hk(Engine& e, u32 ktable_id) { return engine_ktable_now(e, ktable_id)->handle_kind; } // a handle kind, which never changes

// Whether the ship can be hurt now: not by the dials (always, or after the first death),
// not while the egg plays.
bool ship_vulnerable(const Dials& d, const Ship& s) { return (d.invulnerable == 0 || (d.invulnerable == 2 && s.died == 0)) && s.live; }

// The death point is the ship's position, which on_ship_died reads (it writes the ship).
void ship_dies(TickContext& ctx, Ship& s, u32 cause) {
    s.live = 0; // the first hit of the tick; the rest find it gone
    ++s.died;
    ctx.emit(ShipDied{point2{0, 0}, cause, 0});
}

} // namespace

void on_overlap(TickContext& ctx, Span<const OverlapBegin> overlaps) {
    Engine& e = *ctx.engine;
    const u16 ship = hk(e, g_ship_ktable), ring = hk(e, g_ring_ktable), shell = hk(e, g_shell_ktable), shot = hk(e, g_shot_ktable),
              air = hk(e, g_air_ktable), ground = hk(e, g_ground_ktable), plain = hk(e, g_bullet_ktable), active = hk(e, g_active_ktable),
              item = hk(e, g_item_ktable), boss = hk(e, g_boss_ktable), part = hk(e, g_part_ktable), option = hk(e, g_option_ktable);
    for (const OverlapBegin& o : overlaps) {
        for (u32 swap = 0; swap < 2; ++swap) {
            Handle a = swap ? o.b : o.a, b = swap ? o.a : o.b;
            u16 ka = a.kind(), kb = b.kind();
            bool bullet_a = ka == plain || ka == active;
            if (ka == shot && (kb == air || kb == ground || kb == part)) ctx.emit(ShotHit{a, b});
            else if (bullet_a && kb == ship) ctx.emit(BulletHit{a});
            else if ((ka == air || ka == boss) && kb == ship) ctx.emit(Rammed{a});
            else if (bullet_a && kb == ring) ctx.emit(Graze{a});
            else if (bullet_a && (kb == shell || kb == option)) ctx.emit(ShellHit{a}); // an option eats bullets as the shell does
            else if (ka == item && kb == ring) ctx.emit(Pickup{a});
        }
    }
}

bool hurt(TickContext& ctx, Game& g, Handle target, i32 damage, point2 at, bool* killed) {
    Engine& e = *ctx.engine;
    *killed = false;
    const u16 k = target.kind(), air = hk(e, g_air_ktable), ground = hk(e, g_ground_ktable), part = hk(e, g_part_ktable);
    if (k == part) {
        Health& hp = ctx.write<Health>("arcfire.part.Health")[target.index()];
        if (hp.hp <= 0) { *killed = true; return false; } // down already (its trigger takes it): through
        if (part_absorbs(g, ctx, target)) { boss_ping(ctx, g, at); sfx(ctx, g, SFX_PING); return true; }
        hp.hp -= damage;
        hp.flash = kFlashTicks;
        *killed = hp.hp <= 0;
        sfx(ctx, g, SFX_HIT);
        fx_sparks(ctx, g, ctx.read<Transform2>("arcfire.part.Transform2")[target.index()].position, 2, kSparkHit, 2.0f);
        return true;
    }
    if (k == air || k == ground) {
        bool is_air = k == air;
        Health& hp = ctx.write<Health>(is_air ? "arcfire.air.Health" : "arcfire.ground.Health")[target.index()];
        point2 pos = ctx.read<Transform2>(is_air ? "arcfire.air.Transform2" : "arcfire.ground.Transform2")[target.index()].position;
        if (hp.hp <= 0) { *killed = true; return false; } // killed already this tick: through
        hp.hp -= damage;
        hp.flash = kFlashTicks;
        sfx(ctx, g, SFX_HIT);
        fx_sparks(ctx, g, pos, 2, kSparkHit, 2.0f);
        if (hp.hp <= 0) {
            ctx.destroy(target);
            ctx.emit(Kill{pos, hp.score, 0, is_air ? ctx.read<Mover>("arcfire.air.Mover")[target.index()].formation : Handle{}});
            if (!is_air) drop_item(ctx, g, ctx.read<Ground>("arcfire.ground.Ground")[target.index()].drop, pos); // a ground item carrier
            *killed = true;
        }
        return true;
    }
    boss_ping(ctx, g, at); // a boss's hull, which only the beam meets: it stops there, unhurt
    return true;
}

// A shot on an enemy: it hurts it, and goes; a lance goes on through what it kills. A shot
// that met two enemies in one tick hurts the first it is handed, unless that was already
// down (it flies on to the next).
void on_shot_hit(TickContext& ctx, Span<const ShotHit> hits) {
    Game& g = ctx.state_as<Game>();
    Span<Shot> shot = ctx.write<Shot>("arcfire.shot.Shot");
    Span<const Transform2> shot_xf = ctx.read<Transform2>("arcfire.shot.Transform2");
    for (const ShotHit& h : hits) {
        Shot& s = shot[h.shot.index()];
        if (s.spent) continue;
        bool killed = false;
        if (!hurt(ctx, g, h.target, s.damage, shot_xf[h.shot.index()].position, &killed)) continue;
        if (s.kind == ShotLance && killed) continue; // through
        s.spent = 1;
        ctx.destroy(h.shot);
    }
}

// The beam's tick of damage on the first body it met (weapons.cc).
void on_beam_hit(TickContext& ctx, Span<const BeamHit> hits) {
    Game& g = ctx.state_as<Game>();
    for (const BeamHit& h : hits) {
        bool killed = false;
        hurt(ctx, g, h.target, h.damage, h.at, &killed);
        fx_sparks(ctx, g, h.at, 2, color{0.6f, 0.95f, 1.0f, 1.0f}, 2.5f);
    }
}

void on_bullet_hit(TickContext& ctx, Span<const BulletHit> hits) {
    Game& g = ctx.state_as<Game>();
    Span<Ship> ships = ctx.write<Ship>("arcfire.ship.Ship");
    Span<Run> run = ctx.write<Run>(ARC_WORLD(Run));
    Span<const Dials> dials = ctx.read<Dials>(ARC_WORLD(Dials));
    if (g.ship.index() >= ships.len || !run.len) return;
    Ship& s = ships[g.ship.index()];
    for (const BulletHit& h : hits) {
        if (!ship_vulnerable(dials[0], s)) continue;
        Bullet* b = bullet_row(ctx, h.bullet);
        if (!b) continue;
        if (b->grazed == 1) { run[0].score -= kGrazeScore; --run[0].grazes; } // no points for dying
        b->grazed = 2;
        ctx.destroy(h.bullet);
        ship_dies(ctx, s, 0);
    }
}

void on_rammed(TickContext& ctx, Span<const Rammed> rams) {
    Game& g = ctx.state_as<Game>();
    Span<Ship> ships = ctx.write<Ship>("arcfire.ship.Ship");
    Span<const Dials> dials = ctx.read<Dials>(ARC_WORLD(Dials));
    if (g.ship.index() >= ships.len) return;
    Ship& s = ships[g.ship.index()];
    for (u32 i = 0; i < rams.len; ++i)
        if (ship_vulnerable(dials[0], s)) ship_dies(ctx, s, 1);
}

// Through the ring and not the box: once a bullet, ten points and a spark, and the
// chain's window held open a little longer.
void on_graze(TickContext& ctx, Span<const Graze> grazes) {
    Game& g = ctx.state_as<Game>();
    Span<Run> run = ctx.write<Run>(ARC_WORLD(Run));
    Span<const Dials> dials = ctx.read<Dials>(ARC_WORLD(Dials));
    if (!run.len) return;
    for (const Graze& gz : grazes) {
        Bullet* b = bullet_row(ctx, gz.bullet);
        const Transform2* xf = bullet_xf(ctx, gz.bullet);
        if (!b || !xf || b->grazed) continue;
        b->grazed = 1;
        run[0].score += kGrazeScore;
        ++run[0].grazes;
        chain_graze(run[0], dials[0].chain_window);
        sfx(ctx, g, SFX_GRAZE);
        fx_sparks(ctx, g, xf->position, 2, kSparkGraze, 1.5f);
    }
}

// The shell eats the bullet, in a spark where it struck.
void on_shell_hit(TickContext& ctx, Span<const ShellHit> hits) {
    Game& g = ctx.state_as<Game>();
    for (const ShellHit& h : hits) {
        if (const Transform2* xf = bullet_xf(ctx, h.bullet)) fx_sparks(ctx, g, xf->position, 3, kSparkGraze, 2.0f);
        ctx.destroy(h.bullet);
    }
}

// A kill pays, through the chain (a boss's time bonus as it is); a formation's last member
// shot down drops a bomb where it fell.
void on_kill(TickContext& ctx, Span<const Kill> kills) {
    Game& g = ctx.state_as<Game>();
    Span<Run> run = ctx.write<Run>(ARC_WORLD(Run));
    Span<const Dials> dials = ctx.read<Dials>(ARC_WORLD(Dials));
    KTable* formations = ctx.write_ktable(g_formation_ktable);
    if (!run.len) return;
    for (const Kill& k : kills) {
        if (k.flat) run[0].score += k.score;
        else { run[0].score += chain_pay(run[0], dials[0].chain_window, k.score); ++run[0].kills; }
        sfx(ctx, g, k.score >= 400 ? SFX_KILL_BIG : SFX_KILL); // a ground row's or a boss's part's, by its worth
        if (Formation* f = formations->get<Formation>(k.formation)) {
            ++f->killed;
            if (f->killed + f->gone >= f->count) {
                drop_item(ctx, g, f->drop, k.at); // an item carrier's: its last member shot down leaves it
                if (f->gone == 0) {
                    item_drop(ctx, g, ItemBomb, k.at, vec2{0.0f, 1.5f});
                    KV_INFO("arcfire", "formation shot down at tick %llu: a bomb drops", (unsigned long long)ctx.tick);
                }
                formations->destroy(k.formation);
            }
        }
        // Its explosion by its worth: popcorn's, or a ground row's or a part's. A boss's
        // bonus has its own (on_part_down's core).
        if (!k.flat) fx_blast(ctx, g, k.at, k.score >= 400 ? Blast::Medium : Blast::Small);
    }
}

} // namespace kv::arcfire
