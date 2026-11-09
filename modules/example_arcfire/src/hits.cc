// example_arcfire: hits. The collision module reports overlaps between colliders whose
// masks meet; `on_overlap` sorts each pair by its kinds into what it means (a shot on an
// enemy, a bullet on the hitbox, an enemy on the hitbox, a bullet through the graze ring,
// a bullet on the shell), and a handler each does the rest. They write the run in a chain
// (module.cc), so a bullet that hits the hitbox takes back its own graze before the
// grazes are counted.
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

// Whether the ship can be hurt now: not by the dials, not while the egg plays.
bool ship_vulnerable(const Dials& d, const Ship& s) { return !d.invulnerable && s.live; }

// The death point is the ship's position, which on_ship_died reads (it writes the ship).
void ship_dies(TickContext& ctx, Ship& s, u32 cause) {
    s.live = 0; // the first hit of the tick; the rest find it gone
    ctx.emit(ShipDied{point2{0, 0}, cause, 0});
}

} // namespace

void on_overlap(TickContext& ctx, Span<const OverlapBegin> overlaps) {
    Engine& e = *ctx.engine;
    const u16 ship = hk(e, g_ship_kind), ring = hk(e, g_ring_kind), shell = hk(e, g_shell_kind), shot = hk(e, g_shot_kind),
              air = hk(e, g_air_kind), ground = hk(e, g_ground_kind), bullet = hk(e, g_bullet_kind);
    for (const OverlapBegin& o : overlaps) {
        for (u32 swap = 0; swap < 2; ++swap) {
            Handle a = swap ? o.b : o.a, b = swap ? o.a : o.b;
            u16 ka = a.kind(), kb = b.kind();
            if (ka == shot && (kb == air || kb == ground)) ctx.emit(ShotHit{a, b});
            else if (ka == bullet && kb == ship) ctx.emit(BulletHit{a});
            else if (ka == air && kb == ship) ctx.emit(Rammed{a});
            else if (ka == bullet && kb == ring) ctx.emit(Graze{a});
            else if (ka == bullet && kb == shell) ctx.emit(ShellHit{a});
        }
    }
}

// A shot on an enemy: the shot goes, the enemy loses its damage and flashes; at zero it is
// a kill. A shot that met two enemies in one tick hurts the first only.
void on_shot_hit(TickContext& ctx, Span<const ShotHit> hits) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    Span<Shot> shot = ctx.write<Shot>("example_arcfire.shot.Shot");
    Span<Health> air_hp = ctx.write<Health>("example_arcfire.air.Health");
    Span<Health> ground_hp = ctx.write<Health>("example_arcfire.ground.Health");
    Span<const Transform2> air_xf = ctx.read<Transform2>("example_arcfire.air.Transform2");
    Span<const Transform2> ground_xf = ctx.read<Transform2>("example_arcfire.ground.Transform2");
    const u16 air = hk(e, g_air_kind);
    for (const ShotHit& h : hits) {
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
            ctx.emit(Kill{at, hp.score, 0});
        }
    }
}

void on_bullet_hit(TickContext& ctx, Span<const BulletHit> hits) {
    Game& g = ctx.state_as<Game>();
    Span<Bullet> bullet = ctx.write<Bullet>("example_arcfire.bullet.Bullet");
    Span<Ship> ships = ctx.write<Ship>("example_arcfire.ship.Ship");
    Span<Run> run = ctx.write<Run>(ARC_WORLD(Run));
    Span<const Dials> dials = ctx.read<Dials>(ARC_WORLD(Dials));
    if (g.ship.index() >= ships.len || !run.len) return;
    Ship& s = ships[g.ship.index()];
    for (const BulletHit& h : hits) {
        if (!ship_vulnerable(dials[0], s)) continue;
        Bullet& b = bullet[h.bullet.index()];
        if (b.grazed == 1) { run[0].score -= kGrazeScore; --run[0].grazes; } // no points for dying
        b.grazed = 2;
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
    Span<Bullet> bullet = ctx.write<Bullet>("example_arcfire.bullet.Bullet");
    Span<const Transform2> xf = ctx.read<Transform2>("example_arcfire.bullet.Transform2");
    Span<Run> run = ctx.write<Run>(ARC_WORLD(Run));
    if (!run.len) return;
    for (const Graze& gz : grazes) {
        Bullet& b = bullet[gz.bullet.index()];
        if (b.grazed) continue;
        b.grazed = 1;
        run[0].score += kGrazeScore;
        ++run[0].grazes;
        fx_sparks(ctx, g, xf[gz.bullet.index()].position, 2, kSparkGraze, 1.5f);
    }
}

// The shell eats the bullet, in a spark where it struck.
void on_shell_hit(TickContext& ctx, Span<const ShellHit> hits) {
    Game& g = ctx.state_as<Game>();
    Span<const Transform2> xf = ctx.read<Transform2>("example_arcfire.bullet.Transform2");
    for (const ShellHit& h : hits) {
        fx_sparks(ctx, g, xf[h.bullet.index()].position, 3, kSparkGraze, 2.0f);
        ctx.destroy(h.bullet);
    }
}

void on_kill(TickContext& ctx, Span<const Kill> kills) {
    Game& g = ctx.state_as<Game>();
    Span<Run> run = ctx.write<Run>(ARC_WORLD(Run));
    Span<const Dials> dials = ctx.read<Dials>(ARC_WORLD(Dials));
    if (!run.len) return;
    for (const Kill& k : kills) {
        run[0].score += k.score;
        ++run[0].kills;
        fx_sparks(ctx, g, k.at, dials[0].particles * 2, kSparkKill, 4.0f);
        g.shake = g.shake > 3.0f ? g.shake : 3.0f;
    }
}

} // namespace kv::arcfire
