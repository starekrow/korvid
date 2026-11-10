// example_arcfire: the enemies. An air row follows its formation's path by distance and
// starts its pattern once, where its emitter says along the path; a ground row rides the
// scroll and runs its pattern while the ship is above it (it does not turn around).
// Both flash white for the ticks after a hit, by drawing their sprite added instead of
// blended, and both leave the field without a word.
#include "arcfire.h"
#include <math.h>

namespace kv::arcfire {

namespace {

constexpr u32 kBunkerEvery = 240; // ticks between a bunker's launches

// The hit flash: the sprite's own texture, added, for the ticks the flash lasts.
void flash(const Game& g, Health& h, Sprite& s, Tex tex) {
    if (h.flash) { --h.flash; s.material = g.additive[tex]; }
    else s.material = g.alpha[tex];
}

} // namespace

// Phase Sim: movers, emitters started and stopped, the flash. The patterns themselves run
// in `patterns` (patterns.cc), after this.
void enemies(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Span<const Transform2> ship_xf = ctx.read<Transform2>("example_arcfire.ship.Transform2");
    Span<const Stage> stage_row = ctx.read<Stage>(ARC_WORLD(Stage));
    if (!stage_row.len || g.ship.index() >= ship_xf.len) return;
    point2 target = ship_xf[g.ship.index()].position;

    Table* air = ctx.write_kind(g_air_kind);
    Table* formations = ctx.write_kind(g_formation_kind);
    Span<Transform2> at = air->rows_of<Transform2>();
    Span<Mover> mv = air->rows_of<Mover>();
    Span<Emitter> em = air->rows_of<Emitter>();
    Span<Runner> run = air->rows_of<Runner>();
    Span<Health> hp = air->rows_of<Health>();
    Span<Sprite> sp = air->rows_of<Sprite>();
    for (u32 i = 0; i < air->rows(); ++i) {
        if (!air->is_alive(i)) continue;
        Mover& m = mv[i];
        m.s += m.speed;
        if (m.s >= m.path.length) { // away: its formation will not be shot down whole
            if (Formation* f = formations->get<Formation>(m.formation)) {
                if (f->killed + ++f->gone >= f->count) formations->destroy(m.formation);
            }
            ctx.destroy(air->handle_at(i));
            continue;
        }
        at[i].position = path_at(m.path, m.s);
        // Its pattern starts where its formation said along the path, once it is on the field.
        if (em[i].state == EmitIdle && m.s >= em[i].at * m.path.length && in_field(at[i].position, -16.0f))
            emitter_start(g, em[i], run[i], air->handle_at(i), ctx.tick);
        flash(g, hp[i], sp[i], hp[i].look == (u32)Look::Turret ? TEX_TILES : hp[i].look == (u32)Look::Boat ? TEX_BOAT : TEX_SHIPS);
    }

    Table* ground = ctx.write_kind(g_ground_kind);
    Span<Transform2> gt = ground->rows_of<Transform2>();
    Span<Emitter> ge = ground->rows_of<Emitter>();
    Span<Runner> gr = ground->rows_of<Runner>();
    Span<Ground> gg = ground->rows_of<Ground>();
    Span<Health> gh = ground->rows_of<Health>();
    Span<Sprite> gs = ground->rows_of<Sprite>();
    Span<const Run> run_row = ctx.read<Run>(ARC_WORLD(Run));
    Span<const Dials> dials_row = ctx.read<Dials>(ARC_WORLD(Dials));
    f32 speed = stage_row[0].speed;
    for (u32 i = 0; i < ground->rows(); ++i) {
        if (!ground->is_alive(i)) continue;
        point2& p = gt[i].position;
        p.y -= speed * gg[i].rate;
        if (p.y < kFieldBottom - kCullMargin) { ctx.destroy(ground->handle_at(i)); continue; }
        Ground& gk = gg[i];
        if (gk.kind == GroundTank && gk.range > 0.0f) { // along its road, turning at the ends
            p.x += gk.vx;
            if ((gk.vx > 0.0f && p.x > gk.home + gk.range) || (gk.vx < 0.0f && p.x < gk.home - gk.range)) gk.vx = -gk.vx;
        }
        if (gk.kind == GroundBunker && in_field(p, -16.0f) && gk.timer && --gk.timer == 0 && run_row.len) {
            // A launch: a formation of popcorn up out of the bunker, one event a member.
            gk.timer = kBunkerEvery;
            const FormationDef& f = g.stage.formations[gk.launch];
            u32 count = (u32)lroundf((f32)f.count * dials_row[0].formation_count);
            if (count == 0) count = 1;
            Handle group = formations->spawn(Formation{count, 0, 0, 0});
            for (u32 m = 0; m < count; ++m) ctx.schedule(SpawnMember{gk.launch, m, run_row[0].epoch, 0, group, p}, ctx.tick + (u64)m * f.spacing);
        }
        // A ground row fires while the ship is above it (it does not turn round), its pattern
        // again a moment after it ends; the bench's fires whatever the ship does.
        Emitter& e = ge[i];
        bool above = gg[i].bench || (target.y > p.y + 24.0f && p.y < kFieldTop - 8.0f);
        if (!above) { e.state = EmitIdle; e.timer = 0; }
        else if (e.state == EmitDone) { e.state = EmitIdle; e.timer = gg[i].bench ? 60 : 30; }
        else if (e.state == EmitIdle && (e.timer == 0 || --e.timer == 0)) emitter_start(g, e, gr[i], ground->handle_at(i), ctx.tick);
        flash(g, gh[i], gs[i], TEX_TILES);
    }
}

} // namespace kv::arcfire
