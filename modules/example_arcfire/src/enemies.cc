// example_arcfire: the enemies. An air row follows its formation's path by distance and
// fires its aimed shot once, where its emitter says along the path; a ground row rides the
// scroll and fires on its cadence while the ship is above it (it does not turn around).
// Both flash white for the ticks after a hit, by drawing their sprite added instead of
// blended, and both leave the field without a word.
#include "arcfire.h"

namespace kv::arcfire {

namespace {

// The hit flash: the sprite's own texture, added, for the ticks the flash lasts.
void flash(const Game& g, Health& h, Sprite& s, Tex tex) {
    if (h.flash) { --h.flash; s.material = g.additive[tex]; }
    else s.material = g.alpha[tex];
}

} // namespace

// Phase Sim: movers, emitters, the flash. Fires by direct spawn: it holds the bullet write.
void enemies(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Span<const Transform2> ship_xf = ctx.read<Transform2>("example_arcfire.ship.Transform2");
    Span<const Stage> stage_row = ctx.read<Stage>(ARC_WORLD(Stage));
    Span<const Dials> dials_row = ctx.read<Dials>(ARC_WORLD(Dials));
    if (!stage_row.len || g.ship.index() >= ship_xf.len) return;
    const Dials& d = dials_row[0];
    point2 target = ship_xf[g.ship.index()].position;
    Table* bullets_table = ctx.write_kind(g_bullet_kind);
    g.bullets_spawned = 0;

    Table* air = ctx.write_kind(g_air_kind);
    Span<Transform2> at = air->rows_of<Transform2>();
    Span<Mover> mv = air->rows_of<Mover>();
    Span<Emitter> em = air->rows_of<Emitter>();
    Span<Health> hp = air->rows_of<Health>();
    Span<Sprite> sp = air->rows_of<Sprite>();
    for (u32 i = 0; i < air->rows(); ++i) {
        if (!air->is_alive(i)) continue;
        Mover& m = mv[i];
        m.s += m.speed;
        if (m.s >= m.path.length) { ctx.destroy(air->handle_at(i)); continue; }
        at[i].position = path_at(m.path, m.s);
        Emitter& e = em[i];
        if (!e.fired && m.s >= e.at * m.path.length && in_field(at[i].position, -16.0f)) {
            e.fired = 1;
            g.bullets_spawned += bullets_fire_aimed(*bullets_table, g, d, at[i].position, target, e.count, e.speed, air->handle_at(i));
        }
        flash(g, hp[i], sp[i], hp[i].look == (u32)Look::Turret ? TEX_TILES : TEX_SHIPS);
    }

    Table* ground = ctx.write_kind(g_ground_kind);
    Span<Transform2> gt = ground->rows_of<Transform2>();
    Span<Emitter> ge = ground->rows_of<Emitter>();
    Span<Ground> gg = ground->rows_of<Ground>();
    Span<Health> gh = ground->rows_of<Health>();
    Span<Sprite> gs = ground->rows_of<Sprite>();
    f32 speed = stage_row[0].speed;
    for (u32 i = 0; i < ground->rows(); ++i) {
        if (!ground->is_alive(i)) continue;
        point2& p = gt[i].position;
        p.y -= speed * gg[i].rate;
        if (p.y < kFieldBottom - kCullMargin) { ctx.destroy(ground->handle_at(i)); continue; }
        Emitter& e = ge[i];
        if (target.y > p.y + 24.0f && p.y < kFieldTop - 8.0f && ++e.timer >= e.cadence) {
            e.timer = 0;
            g.bullets_spawned += bullets_fire_aimed(*bullets_table, g, d, p, target, e.count, e.speed, ground->handle_at(i));
        }
        flash(g, gh[i], gs[i], TEX_TILES);
    }
}

} // namespace kv::arcfire
