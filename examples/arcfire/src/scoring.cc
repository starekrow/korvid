// arcfire: the score kept and the run's shape. The rules are scoring.h's; here is
// the system that runs the chain's window down, pays the extends and begins a round. The
// run: arcade goes round again at its stage's end a difficulty up, the score carried, and
// ends after Hard; a stage select (--stage) ends with its stage. Either logs the results
// line, shows the banner and quits after it. The demo ends at once.
#include "arcfire.h"
#include "korvid/core/log.h"
#include "korvid/core/format.h"

namespace kv::arcfire {

bool round_next(const Game& g, const Dials& d, Position* next) {
    (void)g;
    if (d.loops) { *next = (Position)d.position; return true; } // the stress run's, the bench's: the same dials again
    return false;
}

bool arcade_next(const Game& g, const Stage& s, const Dials& d, u32* stage, Position* next) {
    if (!g.arcade || d.loops) return false;
    if (s.index + 1 < kStageCount) { *stage = s.index + 1; *next = (Position)d.position; return true; }
    *stage = 0;
    if (d.position == (u32)Position::Easy) { *next = Position::Normal; return true; }
    if (d.position == (u32)Position::Normal) { *next = Position::Hard; return true; }
    return false;
}

RunResult result_of(const Game& g, const Run& r, const Dials& d, u32 round) {
    RunResult out{};
    out.score = r.score;
    format(out.stage, sizeof out.stage, "%s", g.stage.name);
    out.round = round + 1;
    out.position = d.position;
    out.start = (u32)g.start;
    out.restarts = r.restarts;
    out.deaths = r.deaths;
    out.bombs_used = r.bombs_used;
    out.chain_best = r.chain_best;
    out.arcade = g.arcade;
    return out;
}

void results_log(const Game& g, const RunResult& r) {
    (void)g;
    KV_INFO("arcfire", "results: score %lld, %s round %u at %s, %u restarts, %u eggs lost, %u bombs used, longest chain %u", (long long)r.score,
            r.stage, r.round, position_name(r.position), r.restarts, r.deaths, r.bombs_used, r.chain_best);
}

// Phase PreSim, before the sim moves anything: the chain's window runs down; a score past
// the next extend earns its egg; a round the end mark began starts here, at the next
// difficulty for arcade's, so nothing of the old round's tick sees the new dials.
void scoring(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Span<Run> run_row = ctx.write<Run>(ARC_WORLD(Run));
    Span<Stage> stage_row = ctx.write<Stage>(ARC_WORLD(Stage));
    Span<Dials> dials_row = ctx.write<Dials>(ARC_WORLD(Dials));
    if (!run_row.len || !g.stage_ready) return;
    Run& r = run_row[0];
    u32 chain = r.chain;
    if (chain_tick(r) && chain >= 10) KV_INFO("arcfire", "chain: %u kills, x%u, ended at tick %llu", chain, chain_multiplier(chain), (unsigned long long)ctx.tick);
    while (r.score >= r.next_extend) {
        bool egg = egg_gain(r);
        KV_INFO("arcfire", "extend at %lld, tick %llu: %s", (long long)r.next_extend, (unsigned long long)ctx.tick, egg ? "an egg" : "50,000 (five in hand)");
        r.next_extend = extend_after(r.next_extend);
        sfx(ctx, g, SFX_EGG_BLOOM, 1.3f);
    }
    Stage& s = stage_row[0];
    if (!s.round_up) return;
    Dials& d = dials_row[0];
    Position next = (Position)d.position;
    if (!d.loops && round_next(g, d, &next)) {
        d = dials_start(*ctx.engine, g, next);
        dials_log(d);
    }
    s.round_up = 0;
    ++s.loops;
    s.cursor = 0;
    s.ground_cursor = 0;
    s.base = s.distance;
    s.held = 0.0f;
    card_open(*ctx.engine, g, s.loops + 1, d.position);
    KV_INFO("arcfire", "round %u at %s, tick %llu, score %lld", s.loops + 1, position_name(d.position), (unsigned long long)ctx.tick, (long long)r.score);
}

// A run's end, after the banner.
void on_run_over(TickContext& ctx, Span<const RunOver>) { run_over(ctx, ctx.state_as<Game>()); }

} // namespace kv::arcfire
