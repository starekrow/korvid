// arcfire: what the game reports. Every tick it counts what the stress runs read
// (bullets, sprites, the pairs the broadphase tested; the extract's deltas once a frame) into the
// resource `arcfire.stats`, which the HUD's dev counters show; every report
// interval it logs them beside the frame loop's fps line; at the exit, the peaks and the
// means. The notebook (docs/performance-notes.md) is written from these lines.
#include "arcfire.h"
#include "korvid/audio/audio.h"
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
    for (const KTable& t : e.ktables)
        if (t.has(ComponentType<Sprite>::id)) n += t.live();
    return n;
}

} // namespace

// The Frame cycle, after the extract: the Set deltas it sent this frame (a sprite created
// or changed), added up until the next tick's stats take them. The extract runs once a
// frame, and a frame between two ticks sends none, so a count per frame would be mostly 0.
void stats_deltas(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    if (Array<SpriteDelta>* d = sprite_deltas(*ctx.engine))
        for (const SpriteDelta& x : *d) g.frame_deltas += x.kind == SpriteDeltaKind::Set;
}

// Phase PostSim, every tick: the counts of the tick, the peaks and means of the run.
// Diagnostics, on the main thread: it reads the ktables, collision's and audio's figures
// around its declarations, which the schedule cannot order and need not.
void stats(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    Stats& s = g.stats;
    u32 bullets_before = s.bullets;
    s.actives = engine_ktable(e, g_active_ktable)->live();
    s.bullets = engine_ktable(e, g_bullet_ktable)->live() + s.actives;
    s.runners = ctx.paused ? 0 : g.runners_stepped;
    s.fires = s.bullets_spawned;
    s.items = engine_ktable(e, g_item_ktable)->live();
    AudioStats as = audio_stats(e);
    if (as.live_voices > g.voices_peak) g.voices_peak = as.live_voices;
    s.sprites = live_sprites(e);
    s.deltas = g.frame_deltas; // the frames' since the last tick (stats_deltas)
    g.frame_deltas = 0;
    s.bullets_spawned = ctx.paused ? 0 : g.bullets_spawned;
    s.bullets_destroyed = bullets_before + s.bullets_spawned > s.bullets ? bullets_before + s.bullets_spawned - s.bullets : 0;
    if (const CollisionStats* cs = collision_stats(e)) { s.pairs_tested = cs->tested; s.colliders = cs->colliders; s.groups = cs->groups; }
    for (u32 k = 0; k < KV_COUNTOF(s.rows) && k < e.ktables.len; ++k) s.rows[k] = e.ktables[k].live();
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
            s.groups, engine_ktable(e, g_air_ktable)->live(), engine_ktable(e, g_ground_ktable)->live(), engine_ktable(e, g_particle_ktable)->live());
}

namespace {

void stats_summary(Engine& e, Game& g) {
    AudioStats as = audio_stats(e);
    KV_INFO("arcfire", "audio: voices peak %u (the mixer's %u), %u steals, %u starts merged, %u dropped", g.voices_peak, as.mixer.peak_active, as.steals,
            as.merged, as.dropped);
    f64 n = g.sampled ? (f64)g.sampled : 1.0;
    KV_INFO("arcfire", "summary over %llu ticks: bullets peak %u mean %.0f; sprites peak %u mean %.0f; deltas peak %u mean %.0f; "
                       "pairs tested peak %u mean %.0f; colliders peak %u; bullets spawned peak %u a tick; active peak %u; runners peak %u; items peak %u; chips made %u, past the cap %u",
            (unsigned long long)g.sampled, g.peak.bullets, g.sum_bullets / n, g.peak.sprites, g.sum_sprites / n, g.peak.deltas, g.sum_deltas / n,
            g.peak.pairs_tested, g.sum_pairs / n, g.peak.colliders, g.peak.bullets_spawned, g.peak.actives, g.peak.runners, g.peak.items, g.stats.chips_made, g.stats.chips_refused);
}

} // namespace

// The way out. A handler, so a quit asked for while paused still happens; the summary is
// the shutdown's, so the demo's own quit at the stage's end has one too.
void on_exit(TickContext& ctx, Span<const ExitRequested> exits) {
    for (const ExitRequested& x : exits) KV_INFO("arcfire", "exit (%s) at tick %llu", x.reason == ExitReason::Timer ? "timer" : "player", (unsigned long long)ctx.tick);
    engine_request_quit(*ctx.engine);
}

// ---- what this file declares (its part of the module: module.cc lists it) -----------------------
namespace stats_part {
constinit const ResourceDesc k_resources[] = {
    {".stats"},
};
constinit const SystemDesc k_systems[] = {
    {.name = ".stats_deltas", .phase = Phase::Frame, .fn = stats_deltas, .reads = "sprite.deltas", .writes = ".stats", .after = "sprite.extract", .main_thread = true},
    {.name = ".stats", .phase = Phase::PostSim, .fn = stats, .writes = ".stats", .after = ".hud", .main_thread = true,
     .runs = RunWhen::Always},
};
constinit const HandlerDesc k_handlers[] = {
    {.name = ".on_exit", KV_ON(ExitRequested, on_exit), .phase = Phase::Sim},
};

// The run's summary, at the way out.
void shutdown(Engine& e, ModuleInstance& self) {
    Game& g = *self.state_as<Game>();
    stats_summary(e, g);
}
} // namespace stats_part
KV_MODULE_PART(arcfire_stats) = {
    .resources = stats_part::k_resources,
    .systems   = stats_part::k_systems,
    .handlers  = stats_part::k_handlers,
    .shutdown  = stats_part::shutdown,
};

} // namespace kv::arcfire
