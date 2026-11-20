// arcfire: the stage. Distance is the stage's clock: `scroll` adds the speed to it
// every sim tick, and `stage` runs each mark of the stage file as the distance crosses
// it, the one place the game turns distance into events. Nothing reads a tick count to
// know where the stage is, so a death in slow motion or a pause changes when (in wall
// time) a mark comes, never which tick of the stage's own clock it comes on.
#include "arcfire.h"
#include "korvid/asset/blob.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/math/scalar.h"

namespace kv::arcfire {

namespace {

// Every pattern a stage names, found in the library: a name it lacks is the stage's error.
// A formation with no pattern never fires.
bool resolve_stage(const Game& g, StageDef& st, char* why, usize cap) {
    for (u32 i = 0; i < st.formation_count; ++i) {
        FormationDef& f = st.formations[i];
        if (!f.pattern[0]) { f.entry = KV_BML_NONE; continue; }
        if ((f.entry = bml_find(g.lib, f.pattern)) == KV_BML_NONE) { format(why, cap, "no pattern \"%s\"", f.pattern); return false; }
    }
    for (u32 i = 0; i < st.ground_count; ++i) {
        GroundDef& gd = st.grounds[i];
        if ((gd.entry = bml_find(g.lib, gd.pattern)) == KV_BML_NONE) { format(why, cap, "no pattern \"%s\"", gd.pattern); return false; }
    }
    for (u32 i = 0; i < st.map_ground_count; ++i) {
        GroundDef& gd = st.map_grounds[i];
        if ((gd.entry = bml_find(g.lib, gd.pattern)) == KV_BML_NONE) { format(why, cap, "the map's enemy: no pattern \"%s\"", gd.pattern); return false; }
    }
    for (u32 i = 0; i < st.segment_count; ++i) {
        SegmentDef& sd = st.segments[i];
        if ((sd.entry = bml_find(g.lib, sd.pattern)) == KV_BML_NONE) { format(why, cap, "a segment: no pattern \"%s\"", sd.pattern); return false; }
    }
    return true;
}

// The bosses' patterns, and the bench's.
bool resolve_patterns(Game& g, char* why, usize cap) {
    for (u32 b = 0; b < g.boss_count; ++b) {
        BossDef& def = g.bosses[b];
        if (def.stomp[0] && (def.stomp_entry = bml_find(g.lib, def.stomp)) == KV_BML_NONE) {
            format(why, cap, "boss %s: no stomp pattern \"%s\"", def.name, def.stomp);
            return false;
        }
        for (u32 i = 0; i < def.part_count; ++i) {
            PartDef& p = g.bosses[b].parts[i];
            for (u32 k = 0; k < p.pattern_count; ++k)
                if ((p.entries[k] = bml_find(g.lib, p.patterns[k])) == KV_BML_NONE) {
                    format(why, cap, "boss %s, part %s: no pattern \"%s\"", g.bosses[b].name, p.name, p.patterns[k]);
                    return false;
                }
        }
    }
    if (g.bench[0] && (g.bench_entry = bml_find(g.lib, g.bench)) == KV_BML_NONE) { format(why, cap, "--bench: no pattern \"%s\"", g.bench); return false; }
    return true;
}

// The bench (--bench NAME): no marks but a far end, a slow scroll for the planes, and one
// emitter at the top of the field running NAME again and again.
void bench_stage(Game& g, StageDef& st) {
    format(st.name, sizeof st.name, "BENCH %.24s", g.bench);
    st.speed = 0.6f;
    st.length = 1.0e9f;
    st.formation_count = st.ground_count = st.map_ground_count = st.boss_mark_count = st.segment_count = 0;
    st.map[0] = 0;
    st.planes = 7;
    st.mark_count = 1;
    st.marks[0] = Mark{st.length, MarkKind::End, 0, 0.0f, 0.0f};
}

// Where a segment's turrets stand on it, by its cell (segments.png), in world units from
// its middle: the truss's two pads, the module's two.
constexpr vec2 kSegmentSize{192.0f, 96.0f};
constinit const vec2 k_segment_pads[2][2] = {{{-40.0f, 2.0f}, {40.0f, 2.0f}}, {{-52.0f, -16.0f}, {52.0f, -16.0f}}};

} // namespace

// A station segment: its picture a plane row on the ground's layer (the map's, which
// scrolls by the stage's distance, as the ground rows do), placed where the layer's offset
// shows it just above the field; its turrets ground rows on its pads, riding the scroll
// with it. The picture is cosmetic (planes.cc takes it away when the distance jumps back).
void segment_spawn(TickContext& ctx, const Game& g, const Dials& d, const SegmentDef& sd) {
    Span<const Stage> stage_row = ctx.read<Stage>(ARC_WORLD(Stage));
    f32 shown_y = kFieldTop + 24.0f + kSegmentSize.y * 0.5f;
    f32 clock = stage_row.len ? map_clock(stage_row[0]) : 0.0f;
    Sprite s = sprite_of(g, TEX_SEGMENTS, rect2{0.0f, sd.cell ? 0.5f : 0.0f, 1.0f, 0.5f}, kSegmentSize, Z_MAP);
    ctx.spawn_later(g_plane_ktable, transform2_at(point2{sd.x, shown_y + clock}), s, Plane{-1.0f, 0.0f, 0, 0});
    for (u32 i = 0; i < 2; ++i) {
        GroundDef gd{};
        gd.x = sd.x + k_segment_pads[sd.cell][i].x;
        gd.health = sd.health;
        gd.score = sd.score;
        gd.entry = sd.entry;
        gd.kind = GroundTurret;
        gd.drop = i == 0 ? sd.drop : 0;
        ground_spawn(ctx, g, d, gd, point2{gd.x, shown_y + k_segment_pads[sd.cell][i].y});
    }
}

void bench_spawn(TickContext& ctx, Game& g, const Dials& d) {
    GroundDef gd{};
    gd.entry = g.bench_entry;
    ground_spawn(ctx, g, d, gd, point2{0.0f, 200.0f}, true);
    KV_INFO("arcfire", "bench: %s", g.bench);
}

void stage_reset(Stage& s, const StageDef& def) {
    u32 round = s.loops, index = s.index; // a restart is of the round and the stage it happens in
    s = Stage{};
    s.speed = def.speed;
    s.loops = round;
    s.index = index;
}

void stage_enter(TickContext& ctx, Game& g, u32 index, const Dials& d) {
    Engine& e = *ctx.engine;
    if (index >= kStageCount) index = 0;
    g.stage = g.stages[index];
    g.stage_at = index;
    ground_map_show(ctx, g, g.stage.map[0] != 0);
    planes_spawn(e, g, d);
    battle_reset(e, g);
    g.ground_offset = 0.0f;
    KV_INFO("arcfire", "stage %s shown", g.stage.name);
}

// Arcade's stage change (flow_step, on a held tick behind the cover): everything the stage
// made goes; the next stage (the first again, a difficulty up, after the last); the ship,
// its weapon and options, the score and the eggs carried; what the old stage scheduled is
// of an epoch that no longer spawns. A restart in the new stage goes back to its start.
void stage_change(TickContext& ctx, Game& g) {
    Engine& e = *ctx.engine;
    KTable* world = ctx.write_ktable(g_world_ktable);
    Stage* s = world->get<Stage>(g.world);
    Dials* d = world->get<Dials>(g.world);
    Run* r = world->get<Run>(g.world);
    if (!s || !d || !r) return;
    const u32 ktables[] = {g_air_ktable, g_ground_ktable, g_bullet_ktable, g_active_ktable, g_shot_ktable, g_item_ktable, g_formation_ktable, g_boss_ktable, g_part_ktable};
    for (u32 k : ktables) ctx.destroy_all(k);
    u32 loops = s->loops + (g.next_stage <= s->index ? 1u : 0u); // back to the first: a round up
    if ((u32)g.next_position != d->position) {
        *d = dials_start(e, g, g.next_position);
        dials_log(*d);
    }
    stage_enter(ctx, g, g.next_stage, *d);
    stage_reset(*s, g.stage);
    s->loops = loops;
    s->index = g.next_stage;
    ++r->epoch;
    r->stage_score = r->score;
    engine_time_scale(e, 1.0f);
    hud_boss(e, g, nullptr, 0, 0);
    hud_stomp(e, g, false);
    card_open(e, g, loops + 1, d->position);
    KV_INFO("arcfire", "stage %u: %s, round %u at %s, tick %llu, score %lld", s->index + 1, g.stage.name, loops + 1, position_name(d->position),
            (unsigned long long)ctx.tick, (long long)r->score);
}

// Phase PreSim, every tick: until the assets are in, the stage files, the map, the bosses
// and the patterns; then the title or the first run (flow.cc), and the sim may go. After,
// the switches between runs and the title, which want this system's writes.
void boot(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    if (g.stage_ready) { flow_step(ctx, g); return; }
    bool failed = false;
    bool patterns_in = patterns_ready(e, g, failed);
    if (failed) { engine_request_quit(e); return; }
    for (Handle h : g.stage_blobs) {
        const BlobAsset* b = asset_blob(e, h);
        if (!b || b->state == AssetState::Pending) return;
    }
    if (!patterns_in || !g.records_in) return; // the records too: the title shows them
    char why[128] = "the file did not load";
    // The stages, once; then the map (which adds its enemies, and a bunker's formations,
    // to the stage that names it), then every pattern any of them names.
    if (!g.stage_parsed) {
        for (u32 i = 0; i < kStageCount; ++i) {
            const BlobAsset* b = asset_blob(e, g.stage_blobs[i]);
            if (b->state != AssetState::Ready || !stage_parse((const char*)b->data, b->size, g.stages[i], why, sizeof why)) {
                KV_ERROR("arcfire", "stage %s: %s", k_stage_names[i], why);
                engine_request_quit(e);
                return;
            }
            if (g.bench[0]) bench_stage(g, g.stages[i]); // the bench's own: no map, every plane
            if (g.gas_test) g.stages[i].planes |= kPlaneShader;
        }
        g.stage_parsed = true;
    }
    why[0] = 0;
    if (!bosses_ready(e, g, why, sizeof why)) {
        if (why[0]) { KV_ERROR("arcfire", "the bosses: %s", why); engine_request_quit(e); }
        return;
    }
    for (u32 i = 0; i < kStageCount; ++i)
        if (!ground_map_ready(ctx, g, g.stages[i], why, sizeof why)) {
            if (why[0]) { KV_ERROR("arcfire", "stage %s: %s", k_stage_names[i], why); engine_request_quit(e); }
            return;
        }
    for (u32 i = 0; i < kStageCount; ++i)
        if (!resolve_stage(g, g.stages[i], why, sizeof why)) {
            KV_ERROR("arcfire", "stage %s: %s", k_stage_names[i], why);
            engine_request_quit(e);
            return;
        }
    if (!resolve_patterns(g, why, sizeof why)) {
        KV_ERROR("arcfire", "%s", why);
        engine_request_quit(e);
        return;
    }
    g.stage_ready = true;
    for (const StageDef& st : g.stages)
        KV_INFO("arcfire", "stage %s: %u marks, %u formations, %u ground rows, %u segments, %.0f units long, speed %.2f", st.name, st.mark_count,
                st.formation_count, st.ground_count + st.map_ground_count, st.segment_count, (f64)st.length, (f64)st.speed);
    flow_ready(ctx, g);
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
        if (ctx.write_ktable(g_boss_ktable)->live() == 0) { s.waiting = 0; music(ctx, g, false); }
        else s.held += s.speed;
    }
    while (!s.ended && !s.round_up && !s.waiting && s.cursor < def.mark_count && stage_clock(s) >= def.marks[s.cursor].at) {
        const Mark& m = def.marks[s.cursor++];
        switch (m.kind) {
        case MarkKind::Formation: {
            // One event, its members on the ticks the spacing gives from here, so they
            // arrive on time whatever the frame rate or the time scale.
            const FormationDef& f = def.formations[m.index];
            u32 count = (u32)kv::lround((f32)f.count * d.formation_count);
            u32 spacing = (u32)kv::lround((f32)f.spacing * d.formation_spacing);
            if (count == 0) count = 1;
            // Its row first, which counts its members down; then the members, by event.
            Handle group = ctx.write_ktable(g_formation_ktable)->spawn(Formation{count, 0, 0, f.drop});
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
        case MarkKind::Boss: // one boss, or several together
            for (u32 k = 0; k < (u32)m.value; ++k)
                if (!boss_spawn(ctx, g, d, def.boss_of[m.index + k]).is_null()) s.waiting = 1;
            break;
        case MarkKind::Segment:
            segment_spawn(ctx, g, d, def.segments[m.index]);
            break;
        case MarkKind::End:
            ctx.emit(StageEnd{s.loops, 0});
            {
                Position next;
                if (round_next(g, d, &next)) s.round_up = 1; // a looping run: the next round begins at the next tick (scoring.cc)
                else s.ended = 1;                            // the rest: on_stage_end says what comes
            }
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
        Health h{(i32)kv::ceil((f32)f.health * dials_row[0].enemy_health), 0, f.score, (u32)f.look};
        Emitter em{f.entry, f.fire_at, EmitIdle, 0};
        f32 radius = f.look == Look::Boat ? 34.0f : f.look == Look::Heavy ? 26.0f : 18.0f; // the boat, the big ship, the rest
        Sprite s = enemy_sprite(g, f.look);
        if (f.look == Look::Debris) s.uv.x = (f32)(m.member % 3) / 3.0f; // the three pieces in turn
        ctx.spawn_later(g_air_ktable, transform2_at(at, KV_PI), s, collider_circle(radius, L_AIR, L_SHOT | L_SHIP), h, mv, em, Runner{});
    }
}

// The stage's end mark: the stage bonus, and the run goes round again (arcade's next
// difficulty, a stress run's loop) or is over: the results line, and the game quits after
// the banner (the demo at once).
void on_stage_end(TickContext& ctx, Span<const StageEnd> ends) {
    Game& g = ctx.state_as<Game>();
    Span<Run> run_row = ctx.write<Run>(ARC_WORLD(Run));
    Span<const Dials> dials_row = ctx.read<Dials>(ARC_WORLD(Dials));
    Span<const Stage> stage_row = ctx.read<Stage>(ARC_WORLD(Stage));
    for (const StageEnd& se : ends) {
        Run& r = run_row[0];
        const Dials& d = dials_row[0];
        i64 bonus = stage_bonus(r);
        r.score += bonus;
        KV_INFO("arcfire", "stage end: round %u at %s, tick %llu, score %lld (bonus %lld: %d eggs, %d bombs), %u kills, %u grazes, %u deaths, %u restarts",
                se.loops + 1, position_name(d.position), (unsigned long long)ctx.tick, (long long)r.score, (long long)bonus, r.eggs, r.bombs, r.kills,
                r.grazes, r.deaths, r.restarts);
        r.stage_score = r.score;
        // What comes: a looping run's next round (already under way), arcade's next stage
        // (or its first a difficulty up), or the run's end.
        Position next;
        u32 next_stage = 0;
        bool loop = round_next(g, d, &next);
        bool onward = !loop && stage_row.len && arcade_next(g, stage_row[0], d, &next_stage, &next);
        if (!loop && !onward) {
            g.result = result_of(g, r, d, se.loops);
            results_log(g, g.result);
        }
        // The demo ends here (run_over quits, or with --again begins it once more, held, so
        // no sim tick passes between); every run's last hashed tick is this one.
        if (g.demo) { run_over(ctx, g); continue; }
        char label[48] = "";
        if (onward && next_stage > stage_row[0].index) format(label, sizeof label, "NEXT: STAGE %u", next_stage + 1);
        else if (onward || loop) format(label, sizeof label, "NEXT ROUND: %s", position_name((u32)next));
        hud_stage_clear(*ctx.engine, g, bonus, label[0] ? label : nullptr);
        if (onward) ctx.schedule(StageNext{next_stage, (u32)next}, ctx.tick + kRunOverTicks);
        else if (!loop) ctx.schedule(RunOver{se.loops, 0}, ctx.tick + kRunOverTicks);
    }
}

// Arcade's next stage, after the banner: behind a cover, on a held tick (flow.cc).
void on_stage_next(TickContext& ctx, Span<const StageNext> nexts) {
    Game& g = ctx.state_as<Game>();
    for (const StageNext& n : nexts) {
        g.next_stage = n.stage;
        g.next_position = (Position)n.position;
        flow_request(*ctx.engine, g, Next::Stage, true);
    }
}

} // namespace kv::arcfire
