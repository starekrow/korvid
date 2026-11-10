// example_arcfire: what the game reports. Every tick it counts what the stress runs read
// (bullets, sprites, the extract's deltas, the pairs the broadphase tested) into the
// resource `example_arcfire.stats`, which the HUD's dev counters show; every report
// interval it logs them beside the frame loop's fps line; at the exit, the peaks and the
// means. The notebook (docs/performance-notes.md) is written from these lines.
#include "arcfire.h"
#include "korvid/collision/module.h"
#include "korvid/core/log.h"
#include "korvid/core/time.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/sprite/module.h"

namespace kv::arcfire {

namespace {

void peak_of(u32& peak, u32 v) { if (v > peak) peak = v; }

u32 live_sprites(Engine& e) {
    u32 n = 0;
    for (const Table& t : e.tables)
        if (t.has(ComponentType<Sprite>::id)) n += t.live();
    return n;
}

} // namespace

// Phase PostSim, every tick, just before the extract: where its deltas for this tick will
// start (they pile up over the ticks of a frame until the frame loop takes them).
void stats_mark(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Array<SpriteDelta>* d = sprite_deltas(*ctx.engine);
    g.last_delta_len = d ? d->len : 0;
}

// Phase PostSim, every tick, after the extract (it reads what the extract wrote).
void stats(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    Stats& s = g.stats;
    u32 bullets_before = s.bullets;
    s.actives = engine_table(e, g_active_kind)->live();
    s.bullets = engine_table(e, g_bullet_kind)->live() + s.actives;
    s.runners = ctx.paused ? 0 : g.runners_stepped;
    s.fires = s.bullets_spawned;
    s.items = engine_table(e, g_item_kind)->live();
    s.sprites = live_sprites(e);
    s.bullets_spawned = ctx.paused ? 0 : g.bullets_spawned;
    s.bullets_destroyed = bullets_before + s.bullets_spawned > s.bullets ? bullets_before + s.bullets_spawned - s.bullets : 0;
    // This tick's Set deltas: a sprite created or changed.
    s.deltas = 0;
    if (Array<SpriteDelta>* d = sprite_deltas(e))
        for (u32 i = g.last_delta_len; i < d->len; ++i) s.deltas += (*d)[i].kind == SpriteDeltaKind::Set;
    if (const CollisionStats* cs = collision_stats(e)) { s.pairs_tested = cs->tested; s.colliders = cs->colliders; s.groups = cs->groups; }
    for (u32 k = 0; k < KV_COUNTOF(s.rows) && k < e.tables.len; ++k) s.rows[k] = e.tables[k].live();
    if (ctx.paused || !g.stage_ready) return;

    peak_of(g.peak.bullets, s.bullets);
    peak_of(g.peak.sprites, s.sprites);
    peak_of(g.peak.deltas, s.deltas);
    peak_of(g.peak.pairs_tested, s.pairs_tested);
    peak_of(g.peak.colliders, s.colliders);
    peak_of(g.peak.bullets_spawned, s.bullets_spawned);
    peak_of(g.peak.actives, s.actives);
    peak_of(g.peak.runners, s.runners);
    peak_of(g.peak.items, s.items);
    g.sum_bullets += s.bullets;
    g.sum_sprites += s.sprites;
    g.sum_deltas += s.deltas;
    g.sum_pairs += s.pairs_tested;
    ++g.sampled;

    f64 every = (f64)frameloop_config(e).report_every_seconds;
    f64 now = time_now();
    if (every <= 0.0) return;
    if (g.next_report == 0.0) g.next_report = now + every;
    if (now < g.next_report) return;
    g.next_report += every;
    KV_INFO("arcfire", "tick %llu: %u bullets (%u active, +%u -%u), %u runners, %u sprites, %u deltas, %u colliders, %u pairs tested (%u groups), %u air, %u ground, %u particles",
            (unsigned long long)ctx.tick, s.bullets, s.actives, s.bullets_spawned, s.bullets_destroyed, s.runners, s.sprites, s.deltas, s.colliders, s.pairs_tested,
            s.groups, engine_table(e, g_air_kind)->live(), engine_table(e, g_ground_kind)->live(), engine_table(e, g_particle_kind)->live());
}

void stats_summary(Game& g) {
    f64 n = g.sampled ? (f64)g.sampled : 1.0;
    KV_INFO("arcfire", "summary over %llu ticks: bullets peak %u mean %.0f; sprites peak %u mean %.0f; deltas peak %u mean %.0f; "
                       "pairs tested peak %u mean %.0f; colliders peak %u; bullets spawned peak %u a tick; active peak %u; runners peak %u; items peak %u; chips made %u, past the cap %u",
            (unsigned long long)g.sampled, g.peak.bullets, g.sum_bullets / n, g.peak.sprites, g.sum_sprites / n, g.peak.deltas, g.sum_deltas / n,
            g.peak.pairs_tested, g.sum_pairs / n, g.peak.colliders, g.peak.bullets_spawned, g.peak.actives, g.peak.runners, g.peak.items, g.stats.chips_made, g.stats.chips_refused);
}

// The way out. A handler, so a quit asked for while paused still happens; the summary is
// the shutdown's, so the demo's own quit at the stage's end has one too.
void on_exit(TickContext& ctx, Span<const ExitRequested> exits) {
    for (const ExitRequested& x : exits) KV_INFO("arcfire", "exit (%s) at tick %llu", x.reason == ExitReason::Timer ? "timer" : "player", (unsigned long long)ctx.tick);
    engine_request_quit(*ctx.engine);
}

} // namespace kv::arcfire
