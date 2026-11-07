// Events with schemas and handlers: a stale handle drops the event and counts it; a
// handler runs at the boundary on the new batch only; a cascade plays out in waves within
// one tick; the cap stops it and what remains waits for the next boundary; a next-tick
// type is held; two handlers on one event are ordered by their sets.
#include "korvid/core/engine.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

struct Hp { i32 hp; };
constinit const FieldDesc k_hp_fields[] = {KV_FIELD(Hp, hp)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Hp, k_hp_fields)};
constinit const char* const k_rock_components[] = {"Hp"};
constinit const KindDesc k_kinds[] = {{.name = "h.rock", .components = k_rock_components, .capacity = 64}};

struct Hit     { Handle rock; i32 damage; };
struct Split   { Handle rock; u32 depth; };
struct Score   { i32 points; };
struct Chatter { u32 n; };
constinit const FieldDesc k_hit_fields[]   = {KV_FIELD(Hit, rock), KV_FIELD(Hit, damage)};
constinit const FieldDesc k_split_fields[] = {KV_FIELD(Split, rock), KV_FIELD(Split, depth)};
constinit const FieldDesc k_score_fields[] = {KV_FIELD(Score, points)};
constinit const EventTypeDesc k_events[] = {
    KV_EVENT_EX(Hit, k_hit_fields, 0),
    KV_EVENT_EX(Split, k_split_fields, 0),
    KV_EVENT_EX(Score, k_score_fields, EvNextTick),
    KV_EVENT(Chatter),
};

struct Trace {
    u32 hits, splits, scores, chatter_handled, chatter_waves_seen;
    i32 hp_total_seen;   // by the audit handler, after the damage handler
    u32 max_wave;
    u32 scores_in_sim;   // Score events a Sim system saw this tick
    u32 postsim_scores;
    u32 chatter_limit;   // how many Chatter re-emits to make
};
constinit Trace g = {};

// Hit -> damage the rock, emit Split (wave 1). Split -> emit Score and, while depth > 0,
// another Split (waves 2, 3). Score is next-tick: never seen within the tick.
void on_hit(TickContext& ctx, Span<const Hit> hits) {
    Table* rocks = ctx.write_kind("h.rock");
    for (const Hit& h : hits) {
        ++g.hits;
        if (Hp* hp = rocks->get<Hp>(h.rock)) hp->hp -= h.damage;
        ctx.emit(Split{h.rock, 2});
    }
    if (ctx.wave > g.max_wave) g.max_wave = ctx.wave;
}
void on_split(TickContext& ctx, Span<const Split> splits) {
    for (const Split& s : splits) {
        ++g.splits;
        ctx.emit(Score{10});
        if (s.depth > 0) ctx.emit(Split{s.rock, s.depth - 1});
    }
    if (ctx.wave > g.max_wave) g.max_wave = ctx.wave;
}
// Reads the rocks after damage: must be ordered after on_hit by the sets alone.
void on_hit_audit(TickContext& ctx, Span<const Hit>) {
    g.hp_total_seen = 0;
    const Table* rocks = ctx.read_kind("h.rock");
    for (u32 i = 0; i < rocks->rows(); ++i) if (rocks->is_alive(i)) g.hp_total_seen += rocks->rows_of<Hp>()[i].hp; // rows_of on const: fine via get
}
void on_score(TickContext&, Span<const Score> s) { g.scores += s.len; }
// Re-emits itself: an endless cascade the cap must stop.
void on_chatter(TickContext& ctx, Span<const Chatter> cs) {
    ++g.chatter_waves_seen;
    for (const Chatter& c : cs) { ++g.chatter_handled; if (c.n < g.chatter_limit) ctx.emit(Chatter{c.n + 1}); }
}

void sys_sim(TickContext& ctx)     { g.scores_in_sim += ctx.events->read<Score>().len; }
void sys_postsim(TickContext& ctx) { g.postsim_scores += ctx.events->read<Score>().len; }

constinit const char* const k_rock_w[] = {"h.rock"};
constinit const char* const k_rock_r[] = {"h.rock"};
constinit const HandlerDesc k_handlers[] = {
    {.name = "h.audit",   KV_ON(Hit, on_hit_audit), .reads = k_rock_r}, // declared first; ordered after h.damage by the sets
    {.name = "h.damage",  KV_ON(Hit, on_hit),       .writes = k_rock_w},
    {.name = "h.split",   KV_ON(Split, on_split)},
    {.name = "h.score",   KV_ON(Score, on_score)},
    {.name = "h.chatter", KV_ON(Chatter, on_chatter)},
};
constinit const SystemDesc k_systems[] = {
    {.name = "h.sim",     .phase = Phase::Sim,     .fn = sys_sim},
    {.name = "h.postsim", .phase = Phase::PostSim, .fn = sys_postsim},
};
KV_MODULE_DEFINE(k_h) = {.name = "h", .events = k_events, .components = k_components, .kinds = k_kinds,
                         .systems = k_systems, .handlers = k_handlers};

// An event type no module declares: the handler's function type checks, registration does not.
struct Nope { u32 x; };
void on_nope(TickContext&, Span<const Nope>) {}
constinit const HandlerDesc k_bad_handlers[] = {{.name = "bad.h", KV_ON(Nope, on_nope)}};
KV_MODULE_DEFINE(k_bad) = {.name = "bad", .handlers = k_bad_handlers};

} // namespace

void test_handler(kv::TestCtx& c) {
    using namespace kv;
    c.begin("handler");

    const ModuleDesc* const list[] = {&k_h};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    u32 at_damage = 0, at_audit = 0;
    for (u32 i = 0; i < e.handlers.len; ++i) {
        if (strcmp(e.handlers[i].name(), "h.damage") == 0) at_damage = i + 1;
        if (strcmp(e.handlers[i].name(), "h.audit") == 0) at_audit = i + 1;
    }
    KV_CHECK(c, e.handlers.len == 5 && at_damage && at_audit && at_damage < at_audit); // audit after damage, from the sets
    KV_CHECK(c, e.event_types.len == e.event_type_count + 1 && e.event_types[EventType<Hit>::id] == &k_events[0]);

    Table* rocks = engine_table(e, "h.rock");
    Handle r0 = rocks->spawn(Hp{10});
    Handle r1 = rocks->spawn(Hp{10});
    rocks->destroy(r1);

    // Two hits, one naming a dead rock: the stale one is dropped at delivery and counted.
    g = Trace{};
    e.events.emit(Hit{r0, 3});
    e.events.emit(Hit{r1, 3});
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 0);
    KV_CHECK(c, g.hits == 1 && rocks->get<Hp>(r0)->hp == 7);
    KV_CHECK(c, e.events.dropped_stale == 1);
    // audit ran after damage and saw the damaged value.
    KV_CHECK(c, g.hp_total_seen == 7);
    // The cascade: Hit (wave 0) -> Split depth 2 (wave 1) -> depth 1 (wave 2) -> depth 0 (wave 3). Cap is 4.
    KV_CHECK(c, g.splits == 3 && g.max_wave == 3 && e.waves_capped == 0);
    // Score is next-tick: three were emitted, none seen this tick, by handler or system.
    KV_CHECK(c, g.scores == 0 && g.scores_in_sim == 0 && g.postsim_scores == 0);
    KV_CHECK(c, e.events.pending_count() == 3 && e.events.pending_now() == 0);

    // Next tick: the three Scores arrive at the first boundary, the handler and both systems see them.
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 1);
    KV_CHECK(c, g.scores == 3 && g.scores_in_sim == 3 && g.postsim_scores == 3);
    KV_CHECK(c, e.events.pending_count() == 0);

    // An endless cascade: the cap stops it at 4 waves per boundary; what is left waits for
    // the next boundary, so one tick's four boundaries handle 16 and the chain goes on.
    g = Trace{};
    g.chatter_limit = 1000;
    e.events.emit(Chatter{0});
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 2);
    KV_CHECK(c, g.chatter_handled == 16 && g.chatter_waves_seen == 16 && e.waves_capped == 4);
    KV_CHECK(c, e.events.pending_count() == 1); // the 17th waits for the next tick
    g.chatter_limit = 17; // let it end
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 3);
    KV_CHECK(c, g.chatter_handled == 18 && e.events.pending_count() == 0);

    // The trace recorded every delivery with its emitter.
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
    KV_CHECK(c, e.handlers.len == 0 && EventType<Hit>::id == 0);

    // A handler on an undeclared event type is rejected.
    const ModuleDesc* const bad[] = {&k_h, &k_bad};
    Engine f;
    KV_CHECK(c, !engine_init(f, bad));
}
