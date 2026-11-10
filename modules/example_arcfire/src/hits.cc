// example_arcfire: hits. The collision module reports overlaps between colliders whose
// masks meet; `on_overlap` sorts each pair by its kinds into what it means (a shot on an
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

u16 hk(Engine& e, u32 kind_id) { return engine_table(e, kind_id)->kind; }

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
    const u16 ship = hk(e, g_ship_kind), ring = hk(e, g_ring_kind), shell = hk(e, g_shell_kind), shot = hk(e, g_shot_kind),
              air = hk(e, g_air_kind), ground = hk(e, g_ground_kind), plain = hk(e, g_bullet_kind), active = hk(e, g_active_kind),
              item = hk(e, g_item_kind), boss = hk(e, g_boss_kind), part = hk(e, g_part_kind);
    for (const OverlapBegin& o : overlaps) {
        for (u32 swap = 0; swap < 2; ++swap) {
            Handle a = swap ? o.b : o.a, b = swap ? o.a : o.b;
            u16 ka = a.kind(), kb = b.kind();
            bool bullet_a = ka == plain || ka == active;
            if (ka == shot && (kb == air || kb == ground || kb == part)) ctx.emit(ShotHit{a, b});
            else if (bullet_a && kb == ship) ctx.emit(BulletHit{a});
            else if ((ka == air || ka == boss) && kb == ship) ctx.emit(Rammed{a});
            else if (bullet_a && kb == ring) ctx.emit(Graze{a});
            else if (bullet_a && kb == shell) ctx.emit(ShellHit{a});
            else if (ka == item && kb == ring) ctx.emit(Pickup{a});
        }
    }
}

// A shot on an enemy: the shot goes, the enemy loses its damage and flashes; at zero it is
// a kill (a boss's part: its trigger's). A shot that met two enemies in one tick hurts the
// first only.
void on_shot_hit(TickContext& ctx, Span<const ShotHit> hits) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    Span<Shot> shot = ctx.write<Shot>("example_arcfire.shot.Shot");
    Span<const Transform2> shot_xf = ctx.read<Transform2>("example_arcfire.shot.Transform2");
    Span<Health> part_hp = ctx.write<Health>("example_arcfire.part.Health");
    Span<const Transform2> part_xf = ctx.read<Transform2>("example_arcfire.part.Transform2");
    const u16 part = hk(e, g_part_kind);
    for (const ShotHit& h : hits) {
        if (h.target.kind() != part) continue;
        Shot& s = shot[h.shot.index()];
        Health& hp = part_hp[h.target.index()];
        if (s.spent || hp.hp <= 0) continue; // down already: the shot flies on
        s.spent = 1;
        ctx.destroy(h.shot);
        if (part_absorbs(g, ctx, h.target)) { boss_ping(ctx, g, shot_xf[h.shot.index()].position); continue; }
        hp.hp -= s.damage;
        hp.flash = kFlashTicks;
        fx_sparks(ctx, g, part_xf[h.target.index()].position, 2, kSparkHit, 2.0f);
    }
    Span<Health> air_hp = ctx.write<Health>("example_arcfire.air.Health");
    Span<Health> ground_hp = ctx.write<Health>("example_arcfire.ground.Health");
    Span<const Transform2> air_xf = ctx.read<Transform2>("example_arcfire.air.Transform2");
    Span<const Transform2> ground_xf = ctx.read<Transform2>("example_arcfire.ground.Transform2");
    Span<const Mover> air_mv = ctx.read<Mover>("example_arcfire.air.Mover");
    const u16 air = hk(e, g_air_kind);
    for (const ShotHit& h : hits) {
        if (h.target.kind() == part) continue;
        Shot& s = shot[h.shot.index()];
        if (s.spent) continue;
        bool is_air = h.target.kind() == air;
        Health& hp = (is_air ? air_hp : ground_hp)[h.target.index()];
        point2 at = (is_air ? air_xf : ground_xf)[h.target.index()].position;
        if (hp.hp <= 0) continue; // killed already this tick: the shot flies on
        s.spent = 1;
        ctx.destroy(h.shot);
        hp.hp -= s.damage;
        hp.flash = kFlashTicks;
        fx_sparks(ctx, g, at, 2, kSparkHit, 2.0f);
        if (hp.hp <= 0) {
            ctx.destroy(h.target);
            ctx.emit(Kill{at, hp.score, 0, is_air ? air_mv[h.target.index()].formation : Handle{}});
        }
    }
}

void on_bullet_hit(TickContext& ctx, Span<const BulletHit> hits) {
    Game& g = ctx.state_as<Game>();
    Span<Ship> ships = ctx.write<Ship>("example_arcfire.ship.Ship");
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
    Span<Ship> ships = ctx.write<Ship>("example_arcfire.ship.Ship");
    Span<const Dials> dials = ctx.read<Dials>(ARC_WORLD(Dials));
    if (g.ship.index() >= ships.len) return;
    Ship& s = ships[g.ship.index()];
    for (u32 i = 0; i < rams.len; ++i)
        if (ship_vulnerable(dials[0], s)) ship_dies(ctx, s, 1);
}

// Through the ring and not the box: once a bullet, ten points and a spark.
void on_graze(TickContext& ctx, Span<const Graze> grazes) {
    Game& g = ctx.state_as<Game>();
    Span<Run> run = ctx.write<Run>(ARC_WORLD(Run));
    if (!run.len) return;
    for (const Graze& gz : grazes) {
        Bullet* b = bullet_row(ctx, gz.bullet);
        const Transform2* xf = bullet_xf(ctx, gz.bullet);
        if (!b || !xf || b->grazed) continue;
        b->grazed = 1;
        run[0].score += kGrazeScore;
        ++run[0].grazes;
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

// A kill pays; a formation's last member shot down drops a bomb where it fell.
void on_kill(TickContext& ctx, Span<const Kill> kills) {
    Game& g = ctx.state_as<Game>();
    Span<Run> run = ctx.write<Run>(ARC_WORLD(Run));
    Span<const Dials> dials = ctx.read<Dials>(ARC_WORLD(Dials));
    Table* formations = ctx.write_kind(g_formation_kind);
    if (!run.len) return;
    for (const Kill& k : kills) {
        run[0].score += k.score;
        ++run[0].kills;
        if (Formation* f = formations->get<Formation>(k.formation)) {
            ++f->killed;
            if (f->killed + f->gone >= f->count) {
                if (f->gone == 0) {
                    item_drop(ctx, g, ItemBomb, k.at, vec2{0.0f, 1.5f});
                    KV_INFO("arcfire", "formation shot down at tick %llu: a bomb drops", (unsigned long long)ctx.tick);
                }
                formations->destroy(k.formation);
            }
        }
        fx_sparks(ctx, g, k.at, dials[0].particles * 2, kSparkKill, 4.0f);
        g.shake = g.shake > 3.0f ? g.shake : 3.0f;
    }
}

} // namespace kv::arcfire
