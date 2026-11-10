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
#include <stdio.h>

namespace kv::arcfire {

namespace {

// Every pattern the stage names, found in the library: a name it lacks is the stage's error.
bool resolve_patterns(Game& g, char* why, usize cap) {
    for (u32 i = 0; i < g.stage.formation_count; ++i) {
        FormationDef& f = g.stage.formations[i];
        if ((f.entry = bml_find(g.lib, f.pattern)) == KV_BML_NONE) { snprintf(why, cap, "no pattern \"%s\"", f.pattern); return false; }
    }
    for (u32 i = 0; i < g.stage.ground_count; ++i) {
        GroundDef& gd = g.stage.grounds[i];
        if ((gd.entry = bml_find(g.lib, gd.pattern)) == KV_BML_NONE) { snprintf(why, cap, "no pattern \"%s\"", gd.pattern); return false; }
    }
    for (u32 i = 0; i < g.stage.map_ground_count; ++i) {
        GroundDef& gd = g.stage.map_grounds[i];
        if ((gd.entry = bml_find(g.lib, gd.pattern)) == KV_BML_NONE) { snprintf(why, cap, "the map's enemy: no pattern \"%s\"", gd.pattern); return false; }
    }
    for (u32 b = 0; b < g.boss_count; ++b) {
        BossDef& def = g.bosses[b];
        if (def.stomp[0] && (def.stomp_entry = bml_find(g.lib, def.stomp)) == KV_BML_NONE) {
            snprintf(why, cap, "boss %s: no stomp pattern \"%s\"", def.name, def.stomp);
            return false;
        }
        for (u32 i = 0; i < def.part_count; ++i) {
            PartDef& p = g.bosses[b].parts[i];
            for (u32 k = 0; k < p.pattern_count; ++k)
                if ((p.entries[k] = bml_find(g.lib, p.patterns[k])) == KV_BML_NONE) {
                    snprintf(why, cap, "boss %s, part %s: no pattern \"%s\"", g.bosses[b].name, p.name, p.patterns[k]);
                    return false;
                }
        }
    }
    if (g.bench[0] && (g.bench_entry = bml_find(g.lib, g.bench)) == KV_BML_NONE) { snprintf(why, cap, "--bench: no pattern \"%s\"", g.bench); return false; }
    return true;
}

// The bench (--bench NAME): no marks but a far end, a slow scroll for the planes, and one
// emitter at the top of the field running NAME again and again.
void bench_stage(Game& g) {
    StageDef& st = g.stage;
    snprintf(st.name, sizeof st.name, "BENCH %.24s", g.bench);
    st.speed = 0.6f;
    st.length = 1.0e9f;
    st.formation_count = st.ground_count = st.map_ground_count = st.boss_mark_count = 0;
    st.map[0] = 0;
    st.planes = 7;
    st.mark_count = 1;
    st.marks[0] = Mark{st.length, MarkKind::End, 0, 0.0f, 0.0f};
}

void bench_spawn(TickContext& ctx, Game& g, const Dials& d) {
    GroundDef gd{};
    gd.entry = g.bench_entry;
    ground_spawn(ctx, g, d, gd, point2{0.0f, 200.0f}, true);
    KV_INFO("arcfire", "bench: %s", g.bench);
}

} // namespace

void stage_reset(Stage& s, const StageDef& def) {
    s = Stage{};
    s.speed = def.speed;
}

// Phase PreSim, every tick until it has done its work: the stage file, once it is in, and
// the patterns, and the run's first state: the world row, the planes, the ship. Then the
// sim may go.
void boot(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    if (g.stage_ready) return;
    const BlobAsset* b = asset_blob(e, g.stage_blob);
    bool failed = false;
    bool patterns_in = patterns_ready(e, g, failed);
    if (failed) { engine_request_quit(e); return; }
    if (!b || b->state == AssetState::Pending || !patterns_in) return;
    char why[128] = "the file did not load";
    // The stage, once; then its map (which adds its enemies, and a bunker's formations, to
    // the stage), then every pattern either names.
    if (!g.stage_parsed) {
        if (b->state != AssetState::Ready || !stage_parse((const char*)b->data, b->size, g.stage, why, sizeof why)) {
            KV_ERROR("arcfire", "stage %s: %s", b ? b->path : "?", why);
            engine_request_quit(e);
            return;
        }
        g.stage_parsed = true;
        if (g.bench[0]) bench_stage(g); // the bench's own: no map, every plane
    }
    why[0] = 0;
    if (!bosses_ready(e, g, why, sizeof why)) {
        if (why[0]) { KV_ERROR("arcfire", "stage %s: %s", b->path, why); engine_request_quit(e); }
        return;
    }
    if (!ground_map_ready(ctx, g, why, sizeof why)) {
        if (why[0]) { KV_ERROR("arcfire", "stage %s: %s", b->path, why); engine_request_quit(e); }
        return;
    }
    if (!resolve_patterns(g, why, sizeof why)) {
        KV_ERROR("arcfire", "stage %s: %s", b->path, why);
        engine_request_quit(e);
        return;
    }
    g.stage_ready = true;
    KV_INFO("arcfire", "stage %s: %u marks, %u formations, %u ground rows, %.0f units long, speed %.2f", g.stage.name, g.stage.mark_count,
            g.stage.formation_count, g.stage.ground_count, (f64)g.stage.length, (f64)g.stage.speed);

    Dials d = dials_for(g.demo ? Position::Demo : g.stress ? Position::Stress : Position::Normal);
    if (g.autoplay || g.bench[0]) d.bot = 1;
    if (g.bench[0]) { d.invulnerable = 1; d.loops = 1; }
    dials_apply_options(e, d);
    dials_log(d);
    Stage s;
    stage_reset(s, g.stage);
    Run run{};
    run.eggs = 3;
    run.bombs = kBaseBombs;
    Rng r = Rng::seeded(0xa2cf1e5eedull, 1); // the sim's stream: patterns and drift, the same every run
    Table* world = ctx.write_kind(g_world_kind);
    g.world = world->spawn(d, s, run, EggState{}, Chance{r.state, r.inc});
    planes_spawn(e, g, d);
    g.ship = ship_spawn(e, g);
    if (g.bench[0]) bench_spawn(ctx, g, d);
    music(ctx, g, false);
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

// Phase Sim, after the scroll: every mark the stage's clock has passed, in order.
void stage(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Span<Stage> stage_row = ctx.write<Stage>(ARC_WORLD(Stage));
    Span<const Dials> dials_row = ctx.read<Dials>(ARC_WORLD(Dials));
    Span<const Run> run_row = ctx.read<Run>(ARC_WORLD(Run));
    if (!stage_row.len || !g.stage_ready) return;
    Stage& s = stage_row[0];
    const Dials& d = dials_row[0];
    const StageDef& def = g.stage;
    // The map's ground rows whose place has come into view: where the map has them now.
    f32 along = map_clock(s);
    while (s.ground_cursor < def.map_ground_count && along >= def.map_grounds[s.ground_cursor].at) {
        const GroundDef& gd = def.map_grounds[s.ground_cursor++];
        ground_spawn(ctx, g, d, gd, point2{gd.x, kFieldTop + 24.0f - (along - gd.at)});
    }
    // A boss mark holds the marks' clock until its boss has died or left; the scroll goes on.
    if (s.waiting) {
        if (ctx.write_kind(g_boss_kind)->live() == 0) { s.waiting = 0; music(ctx, g, false); }
        else s.held += s.speed;
    }
    while (!s.ended && !s.waiting && s.cursor < def.mark_count && stage_clock(s) >= def.marks[s.cursor].at) {
        const Mark& m = def.marks[s.cursor++];
        switch (m.kind) {
        case MarkKind::Formation: {
            // One event, its members on the ticks the spacing gives from here, so they
            // arrive on time whatever the frame rate or the time scale.
            const FormationDef& f = def.formations[m.index];
            u32 count = (u32)lroundf((f32)f.count * d.formation_count);
            u32 spacing = (u32)lroundf((f32)f.spacing * d.formation_spacing);
            if (count == 0) count = 1;
            // Its row first, which counts its members down; then the members, by event.
            Handle group = ctx.write_kind(g_formation_kind)->spawn(Formation{count, 0, 0, 0});
            for (u32 i = 0; i < count; ++i) ctx.schedule(SpawnMember{m.index, i, run_row[0].epoch, 0, group, point2{0, 0}}, ctx.tick + (u64)i * spacing);
            break;
        }
        case MarkKind::Ground:
            ground_spawn(ctx, g, d, def.grounds[m.index], point2{def.grounds[m.index].x, kFieldTop + 24.0f});
            break;
        case MarkKind::Speed:
            if (m.over > 0.0f) { s.ramp_from = s.speed; s.ramp_to = m.value; s.ramp_at = s.distance; s.ramp_over = m.over; }
            else { s.speed = m.value; s.ramp_over = 0.0f; }
            break;
        case MarkKind::Boss:
            if (!boss_spawn(ctx, g, d, g.boss_of[m.index]).is_null()) s.waiting = 1;
            break;
        case MarkKind::End:
            ctx.emit(StageEnd{s.loops, 0});
            if (d.loops) { ++s.loops; s.cursor = 0; s.ground_cursor = 0; s.base = s.distance; s.held = 0.0f; }
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
        mv.formation = m.group;
        if (m.offset.x != 0.0f || m.offset.y != 0.0f) // a bunker's launch: the path starts where the bunker is
            for (u32 k = 0; k < mv.path.count; ++k) mv.path.pts[k] += vec2{m.offset.x, m.offset.y};
        point2 at = path_at(mv.path, 0.0f);
        Health h{(i32)ceilf((f32)f.health * dials_row[0].enemy_health), 0, f.score, (u32)f.look};
        Emitter em{f.entry, f.fire_at, EmitIdle, 0};
        ctx.spawn_later(g_air_kind, transform2_at(at, KV_PI), enemy_sprite(g, f.look), collider_circle(18.0f, L_AIR, L_SHOT | L_SHIP), h, mv, em, Runner{});
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
        // The demo ends here; the engine runs no tick after a quit, so every run's last
        // hashed tick is this one.
        if (g.demo) engine_request_quit(*ctx.engine);
        else hud_stage_clear(*ctx.engine, g);
    }
}

} // namespace kv::arcfire
