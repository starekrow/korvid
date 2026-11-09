// example_arcfire: the stage. Distance is the stage's clock: `scroll` adds the speed to it
// every sim tick, and `stage` runs each mark of the stage file as the distance crosses
// it, the one place the game turns distance into events. Nothing reads a tick count to
// know where the stage is, so a death in slow motion or a pause changes when (in wall
// time) a mark comes, never which tick of the stage's own clock it comes on.
#include "arcfire.h"
#include "korvid/asset/blob.h"
#include "korvid/core/log.h"
#include "korvid/frameloop/frameloop.h"
#include <math.h>

namespace kv::arcfire {

void stage_reset(Stage& s, const StageDef& def) {
    s = Stage{};
    s.speed = def.speed;
}

// Phase PreSim, every tick until it has done its work: the stage file, once it is in, and
// the run's first state: the world row, the planes, the ship. Then the sim may go.
void boot(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    if (g.stage_ready) return;
    const BlobAsset* b = asset_blob(e, g.stage_blob);
    if (!b || b->state == AssetState::Pending) return;
    char why[128] = "the file did not load";
    if (b->state != AssetState::Ready || !stage_parse((const char*)b->data, b->size, g.stage, why, sizeof why)) {
        KV_ERROR("arcfire", "stage %s: %s", b ? b->path : "?", why);
        engine_request_quit(e);
        return;
    }
    g.stage_ready = true;
    KV_INFO("arcfire", "stage %s: %u marks, %u formations, %u ground rows, %.0f units long, speed %.2f", g.stage.name, g.stage.mark_count,
            g.stage.formation_count, g.stage.ground_count, (f64)g.stage.length, (f64)g.stage.speed);

    Dials d = dials_for(g.demo ? Position::Demo : g.stress ? Position::Stress : Position::Normal);
    if (g.autoplay) d.bot = 1;
    dials_apply_options(e, d);
    dials_log(d);
    Stage s;
    stage_reset(s, g.stage);
    Run run{};
    run.eggs = 3;
    Rng r = Rng::seeded(0xa2cf1e5eedull, 1); // the sim's stream: patterns and drift, the same every run
    Table* world = ctx.write_kind(g_world_kind);
    g.world = world->spawn(d, s, run, EggState{}, Chance{r.state, r.inc});
    planes_spawn(e, g, d);
    g.ship = ship_spawn(e, g);
    engine_hold(e, HoldLoading, false);
}

// Phase Sim: the distance, at the speed, which ramps where a speed mark said.
void scroll(TickContext& ctx) {
    Span<Stage> stage_row = ctx.write<Stage>(ARC_WORLD(Stage));
    if (!stage_row.len) return;
    Stage& s = stage_row[0];
    if (s.ramp_over > 0.0f) {
        f32 t = (s.distance - s.ramp_at) / s.ramp_over;
        if (t >= 1.0f) { s.speed = s.ramp_to; s.ramp_over = 0.0f; }
        else s.speed = lerp(s.ramp_from, s.ramp_to, t < 0.0f ? 0.0f : t);
    }
    s.distance += s.speed;
}

// Phase Sim, after the scroll: every mark the distance has passed, in order.
void stage(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Span<Stage> stage_row = ctx.write<Stage>(ARC_WORLD(Stage));
    Span<const Dials> dials_row = ctx.read<Dials>(ARC_WORLD(Dials));
    Span<const Run> run_row = ctx.read<Run>(ARC_WORLD(Run));
    if (!stage_row.len || !g.stage_ready) return;
    Stage& s = stage_row[0];
    const Dials& d = dials_row[0];
    const StageDef& def = g.stage;
    // A loop's marks are offset by the stage's length times the loops so far.
    while (!s.ended && s.cursor < def.mark_count && s.distance >= (f32)s.loops * def.length + def.marks[s.cursor].at) {
        const Mark& m = def.marks[s.cursor++];
        switch (m.kind) {
        case MarkKind::Formation: {
            // One event, its members on the ticks the spacing gives from here, so they
            // arrive on time whatever the frame rate or the time scale.
            const FormationDef& f = def.formations[m.index];
            u32 count = (u32)lroundf((f32)f.count * d.formation_count);
            u32 spacing = (u32)lroundf((f32)f.spacing * d.formation_spacing);
            if (count == 0) count = 1;
            for (u32 i = 0; i < count; ++i) ctx.schedule(SpawnMember{m.index, i, run_row[0].epoch, 0}, ctx.tick + (u64)i * spacing);
            break;
        }
        case MarkKind::Ground: {
            const GroundDef& gd = def.grounds[m.index];
            Health h{(i32)ceilf((f32)gd.health * d.enemy_health), 0, gd.score, (u32)Look::Turret};
            Emitter em{0.0f, 1, gd.cadence, gd.cadence / 2, 0, gd.speed};
            ctx.spawn_later(g_ground_kind, transform2_at(point2{gd.x, kFieldTop + 24.0f}), enemy_sprite(g, Look::Turret),
                            collider_circle(14.0f, L_GROUND, L_SHOT), h, em, Ground{1.0f, 0});
            break;
        }
        case MarkKind::Speed:
            if (m.over > 0.0f) { s.ramp_from = s.speed; s.ramp_to = m.value; s.ramp_at = s.distance; s.ramp_over = m.over; }
            else { s.speed = m.value; s.ramp_over = 0.0f; }
            break;
        case MarkKind::End:
            ctx.emit(StageEnd{s.loops, 0});
            if (d.loops) { ++s.loops; s.cursor = 0; }
            else s.ended = 1;
            break;
        }
    }
}

// A formation's member, on its tick: at the start of the formation's path, facing down.
// A member scheduled before a restart belongs to a stage that is gone.
void on_member(TickContext& ctx, Span<const SpawnMember> members) {
    Game& g = ctx.state_as<Game>();
    Span<const Run> run_row = ctx.read<Run>(ARC_WORLD(Run));
    Span<const Dials> dials_row = ctx.read<Dials>(ARC_WORLD(Dials));
    if (!run_row.len) return;
    for (const SpawnMember& m : members) {
        if (m.epoch != run_row[0].epoch || m.formation >= g.stage.formation_count) continue;
        const FormationDef& f = g.stage.formations[m.formation];
        Mover mv;
        mv.path = g.stage.paths[f.path];
        mv.speed = f.speed;
        mv.s = 0.0f;
        point2 at = path_at(mv.path, 0.0f);
        Health h{(i32)ceilf((f32)f.health * dials_row[0].enemy_health), 0, f.score, (u32)f.look};
        Emitter em{f.fire_at, f.fire_count, 0, 0, 0, 3.5f};
        ctx.spawn_later(g_air_kind, transform2_at(at, KV_PI), enemy_sprite(g, f.look), collider_circle(18.0f, L_AIR, L_SHOT | L_SHIP), h, mv, em);
    }
}

// The stage's end mark: the demo is done; play and stress go round again from the
// score as it stands.
void on_stage_end(TickContext& ctx, Span<const StageEnd> ends) {
    Game& g = ctx.state_as<Game>();
    Span<Run> run_row = ctx.write<Run>(ARC_WORLD(Run));
    for (const StageEnd& se : ends) {
        Run& r = run_row[0];
        KV_INFO("arcfire", "stage end: loop %u at tick %llu, score %lld, %u kills, %u grazes, %u deaths, %u restarts", se.loops,
                (unsigned long long)ctx.tick, (long long)r.score, r.kills, r.grazes, r.deaths, r.restarts);
        r.stage_score = r.score;
        // The demo ends here. The quit lands at the frame's end, and a frame may hold more
        // ticks than this one: the hold makes the rest paused ones, so every run's last
        // hashed tick is this one.
        if (g.demo) { engine_hold(*ctx.engine, HoldGame, true); engine_request_quit(*ctx.engine); }
        else hud_stage_clear(*ctx.engine, g);
    }
}

} // namespace kv::arcfire
