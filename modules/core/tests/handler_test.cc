// Events in the schedule: handlers are systems with a phase, ordered by their sets; a
// chain of events resolves inside one phase in data order; a loop reads early and goes
// round once a tick, with no cap; a next-tick type is held; every consumer sees every
// event once; emitters in one level land in rank order; a stale handle drops the event
// from the batch; cycles, a missing phase and an undeclared event type are refused.
#include "korvid/core/engine.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

struct Hp { i32 hp; };
constinit const FieldDesc k_hp_fields[] = {KV_FIELD(Hp, hp)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Hp, k_hp_fields)};
constinit const KindDesc k_kinds[] = {{.name = "h.rock", .components = "Hp", .capacity = 64}};

struct Hit     { Handle rock; i32 damage; };
struct Split   { Handle rock; u32 depth; };
struct Tally   { u32 n; };
struct Score   { i32 points; };
struct Chatter { u32 n; };
struct Ping    { u32 from; };
constinit const FieldDesc k_hit_fields[]   = {KV_FIELD(Hit, rock), KV_FIELD(Hit, damage)};
constinit const FieldDesc k_split_fields[] = {KV_FIELD(Split, rock), KV_FIELD(Split, depth)};
constinit const FieldDesc k_score_fields[] = {KV_FIELD(Score, points)};
constinit const EventTypeDesc k_events[] = {
    KV_EVENT_EX(Hit, k_hit_fields, 0),
    KV_EVENT_EX(Split, k_split_fields, 0),
    KV_EVENT(Tally),
    KV_EVENT_EX(Score, k_score_fields, EvNextTick),
    KV_EVENT(Chatter),
    KV_EVENT(Ping),
};

struct Trace {
    u32 hits, splits, tallies, scores;
    i32 hp_total_seen;   // by the audit handler, after the damage handler
    u32 scores_in_sim;   // Score events a Sim system received this tick
    u32 postsim_scores;
    u32 chatter_handled, chatter_runs;
    u32 chatter_limit;   // how many Chatter re-emits to make
    u32 pings[8], nping;
};
constinit Trace g = {};

// Hit -> damage the rock, emit Split. Split -> emit a Tally and a Score. All in Sim: the
// chain is over by the end of the phase.
void on_hit(TickContext& ctx, Span<const Hit> hits) {
    Table* rocks = ctx.write_kind("h.rock");
    for (const Hit& h : hits) {
        ++g.hits;
        if (Hp* hp = rocks->get<Hp>(h.rock)) hp->hp -= h.damage;
        ctx.emit(Split{h.rock, 2});
    }
}
void on_split(TickContext& ctx, Span<const Split> splits) {
    for (const Split& s : splits) { ++g.splits; ctx.emit(Tally{s.depth}); ctx.emit(Score{10}); }
}
void on_tally(TickContext&, Span<const Tally> ts) { g.tallies += ts.len; }
// Reads the rocks after damage: must be ordered after on_hit by the sets alone.
void on_hit_audit(TickContext& ctx, Span<const Hit>) {
    g.hp_total_seen = 0;
    const Table* rocks = ctx.read_kind("h.rock");
    for (u32 i = 0; i < rocks->rows(); ++i) if (rocks->is_alive(i)) g.hp_total_seen += rocks->rows_of<Hp>()[i].hp;
}
void on_score(TickContext&, Span<const Score> s) { g.scores += s.len; }
// Re-emits itself: it reads its own type early, so what it emits is next tick's.
void on_chatter(TickContext& ctx, Span<const Chatter> cs) {
    ++g.chatter_runs;
    for (const Chatter& c : cs) { ++g.chatter_handled; if (c.n < g.chatter_limit) ctx.emit(Chatter{c.n + 1}); }
}
void on_ping(TickContext&, Span<const Ping> ps) { for (const Ping& p : ps) if (g.nping < 8) g.pings[g.nping++] = p.from; }

void sys_sim(TickContext& ctx)     { g.scores_in_sim += ctx.received<Score>().len; }
void sys_postsim(TickContext& ctx) { g.postsim_scores += ctx.received<Score>().len; }
// Two emitters with nothing between them, declared second-first so the later rank runs
// in the same level: the consumer sees them in rank order all the same.
void sys_ping_b(TickContext& ctx) { ctx.emit(Ping{2}); }
void sys_ping_a(TickContext& ctx) { ctx.emit(Ping{1}); }

constinit const SystemDesc k_systems[] = {
    {.name = "h.ping_a",  .phase = Phase::PreSim,  .fn = sys_ping_a, .emits = "Ping", .state = StateUse::None},
    {.name = "h.ping_b",  .phase = Phase::PreSim,  .fn = sys_ping_b, .emits = "Ping", .state = StateUse::None},
    {.name = "h.sim",     .phase = Phase::Sim,     .fn = sys_sim,     .reads = "Score", .state = StateUse::None},
    {.name = "h.postsim", .phase = Phase::PostSim, .fn = sys_postsim, .reads = "Score", .state = StateUse::None},
};
constinit const HandlerDesc k_handlers[] = {
    {.name = "h.audit",   KV_ON(Hit, on_hit_audit), .phase = Phase::Sim, .reads = "h.rock"}, // declared first; after h.damage by the sets
    {.name = "h.damage",  KV_ON(Hit, on_hit),       .phase = Phase::Sim, .writes = "h.rock", .emits = "Split"},
    {.name = "h.split",   KV_ON(Split, on_split),   .phase = Phase::Sim, .emits = "Tally, Score"},
    {.name = "h.tally",   KV_ON(Tally, on_tally),   .phase = Phase::Sim},
    {.name = "h.score",   KV_ON(Score, on_score),   .phase = Phase::Sim},
    {.name = "h.chatter", KV_ON(Chatter, on_chatter), .phase = Phase::Sim, .reads = "^Chatter", .emits = "Chatter"},
    {.name = "h.ping",    KV_ON(Ping, on_ping),     .phase = Phase::PreSim},
};
KV_MODULE_DEFINE(k_h) = {.name = "h", .events = k_events, .components = k_components, .kinds = k_kinds,
                         .systems = k_systems, .handlers = k_handlers};

// An event type no module declares: the handler's function type checks, registration does not.
struct Nope { u32 x; };
void on_nope(TickContext&, Span<const Nope>) {}
constinit const HandlerDesc k_bad_handlers[] = {{.name = "bad.h", KV_ON(Nope, on_nope), .phase = Phase::Sim}};
KV_MODULE_DEFINE(k_bad) = {.name = "bad", .handlers = k_bad_handlers};

// No phase; a loop in one phase; a handler emitting what it handles, read as it stands.
constinit const HandlerDesc k_nophase_handlers[] = {{.name = "np.h", KV_ON(Tally, on_tally)}};
KV_MODULE_DEFINE(k_nophase) = {.name = "np", .handlers = k_nophase_handlers};
void on_tally_ping(TickContext& ctx, Span<const Tally>) { ctx.emit(Ping{0}); }
void on_ping_tally(TickContext& ctx, Span<const Ping>) { ctx.emit(Tally{0}); }
constinit const HandlerDesc k_loop_handlers[] = {
    {.name = "lp.a", KV_ON(Tally, on_tally_ping), .phase = Phase::Sim, .emits = "Ping"},
    {.name = "lp.b", KV_ON(Ping, on_ping_tally), .phase = Phase::Sim, .emits = "Tally"},
};
KV_MODULE_DEFINE(k_loop) = {.name = "lp", .handlers = k_loop_handlers};
constinit const HandlerDesc k_self_handlers[] = {{.name = "sf.h", KV_ON(Chatter, on_chatter), .phase = Phase::Sim, .emits = "Chatter"}};
KV_MODULE_DEFINE(k_self) = {.name = "sf", .handlers = k_self_handlers};

} // namespace

void test_handler(kv::TestCtx& c) {
    using namespace kv;
    c.begin("handler");

    const ModuleDesc* const list[] = {&k_h};
    Engine e;
    e.threads = 4;
    KV_CHECK(c, engine_init(e, list));
    const Array<ScheduledSystem>& sim = e.phases.systems[(u32)Phase::Sim];
    u32 at_damage = 0, at_audit = 0, at_split = 0, at_tally = 0;
    for (u32 i = 0; i < sim.len; ++i) {
        if (strcmp(sim[i].name(), "h.damage") == 0) at_damage = i + 1;
        if (strcmp(sim[i].name(), "h.audit") == 0) at_audit = i + 1;
        if (strcmp(sim[i].name(), "h.split") == 0) at_split = i + 1;
        if (strcmp(sim[i].name(), "h.tally") == 0) at_tally = i + 1;
    }
    KV_CHECK(c, at_damage && at_audit && at_damage < at_audit);              // audit after damage, from the sets
    KV_CHECK(c, at_damage < at_split && at_split < at_tally);                // the chain, from the streams
    KV_CHECK(c, e.event_types.len == e.event_type_count + 1 && e.event_types[EventType<Hit>::id] == &k_events[0]);

    Table* rocks = engine_table(e, "h.rock");
    Handle r0 = rocks->spawn(Hp{10});
    Handle r1 = rocks->spawn(Hp{10});
    rocks->destroy(r1);

    // Two hits from outside, one naming a dead rock: the stale one is dropped from the
    // batch and counted. The chain runs out inside Sim.
    g = Trace{};
    e.events.emit(Hit{r0, 3});
    e.events.emit(Hit{r1, 3});
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 0);
    KV_CHECK(c, g.hits == 1 && rocks->get<Hp>(r0)->hp == 7);
    KV_CHECK(c, e.events.dropped_stale == 2); // from each consumer's batch: damage's and audit's
    KV_CHECK(c, g.hp_total_seen == 7);                 // audit ran after damage and saw the damaged value
    KV_CHECK(c, g.splits == 1 && g.tallies == 1);      // Hit -> Split -> Tally, one phase
    // Score is next-tick: one emitted, none received this tick, by handler or system.
    KV_CHECK(c, g.scores == 0 && g.scores_in_sim == 0 && g.postsim_scores == 0);
    // The two emitters of one level, in rank order whatever ran first.
    KV_CHECK(c, g.nping == 2 && g.pings[0] == 1 && g.pings[1] == 2);

    // Next tick: the Score is in the tick's start; the handler and both systems receive it,
    // once each.
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 1);
    KV_CHECK(c, g.scores == 1 && g.scores_in_sim == 1 && g.postsim_scores == 1);
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 2);
    KV_CHECK(c, g.scores == 1 && g.scores_in_sim == 1 && g.postsim_scores == 1);

    // A loop: one step a tick, as long as it goes, nothing capped or carried.
    g = Trace{};
    g.chatter_limit = 1000;
    e.events.emit(Chatter{0});
    for (u64 t = 3; t < 23; ++t) e.phases.run_tick(e, e.events, 1.0 / 60.0, t);
    KV_CHECK(c, g.chatter_handled == 20 && g.chatter_runs == 20);
    g.chatter_limit = 0; // let it end
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 23);
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 24);
    KV_CHECK(c, g.chatter_handled == 21 && g.chatter_runs == 21);

    // The trace recorded every merge with its emitter.
#if KV_DEV
    KV_CHECK(c, e.trace.total > 0);
    bool saw_split_from_damage = false;
    for (const TraceRecord& r : e.trace.ring)
        if (r.type == EventType<Split>::id && r.emitter && strcmp(r.emitter, "h.damage") == 0) saw_split_from_damage = true;
    KV_CHECK(c, saw_split_from_damage);
    char text[128];
    Hit sample{r0, 3};
    engine_describe_event(e, EventType<Hit>::id, &sample, text, sizeof text);
    KV_CHECK(c, strncmp(text, "Hit{rock=h.rock#0, damage=3}", sizeof text) == 0);
#endif

    engine_shutdown(e);
    KV_CHECK(c, e.phases.system_count() == 0 && EventType<Hit>::id == 0);

    // Refused: a handler on an undeclared event type; one with no phase; two handlers that
    // feed each other in one phase; a handler emitting what it handles, read as it stands.
    const ModuleDesc* const bad[] = {&k_h, &k_bad};
    Engine f;
    KV_CHECK(c, !engine_init(f, bad));
    const ModuleDesc* const nophase[] = {&k_h, &k_nophase};
    KV_CHECK(c, !engine_init(f, nophase));
    const ModuleDesc* const loop[] = {&k_h, &k_loop};
    KV_CHECK(c, !engine_init(f, loop));
    const ModuleDesc* const self[] = {&k_h, &k_self};
    KV_CHECK(c, !engine_init(f, self));
}
