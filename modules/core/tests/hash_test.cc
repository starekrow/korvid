// The state hash: the same world hashes the same however its kinds were numbered. Two
// engines with the modules in opposite orders, and a third with an extra module ahead of
// them, give their kinds different ids; a handle in a field still hashes the same.
#include "korvid/core/engine.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"

namespace {

using namespace kv;

struct HPos { f32 x, y; };
struct HRef { Handle other; u32 n; };

constinit const FieldDesc k_pos_fields[] = {KV_FIELDS(HPos, x, y)};
constinit const FieldDesc k_ref_fields[] = {KV_FIELDS(HRef, other, n)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(HPos, k_pos_fields), KV_COMPONENT(HRef, k_ref_fields)};

constinit const KindDesc k_a_kinds[] = {{.name = "ha.thing", .components = "HPos, HRef", .capacity = 8}};
constinit const KindDesc k_b_kinds[] = {{.name = "hb.mark", .components = "HPos", .capacity = 8}};
constinit const KindDesc k_x_kinds[] = {{.name = "hx.extra", .components = "HPos", .capacity = 8}, {.name = "hx.more", .components = "HPos", .capacity = 8}};
KV_MODULE_DEFINE(k_ha) = {.name = "ha", .components = k_components, .kinds = k_a_kinds};
KV_MODULE_DEFINE(k_hb) = {.name = "hb", .kinds = k_b_kinds};
KV_MODULE_DEFINE(k_hx) = {.name = "hx", .kinds = k_x_kinds};

// The same world in any engine: two marks, a thing pointing at the second.
u64 world_hash(Engine& e, f32 nudge, u16* mark_kind) {
    Table* marks = engine_table(e, "hb.mark");
    Table* things = engine_table(e, "ha.thing");
    marks->spawn(HPos{1, 2});
    Handle m = marks->spawn(HPos{3, 4});
    things->spawn(HPos{5 + nudge, 6}, HRef{m, 7});
    *mark_kind = marks->kind;
    return engine_hash_state(e);
}

// The rest of the sim state: a module's ctx.rng() stream, a published resource with a
// field table, a timer waiting in the wheel.
struct HScore { i32 total; u32 draws; };
struct HPing { u32 n; };
constinit const FieldDesc k_score_fields[] = {KV_FIELDS(HScore, total, draws)};
constinit const FieldDesc k_ping_fields[] = {KV_FIELDS(HPing, n)};
constinit const EventTypeDesc k_hs_events[] = {KV_EVENT_EX(HPing, k_ping_fields, 0)};
constinit const ResourceDesc k_hs_resources[] = {{.name = ".score", .fields = k_score_fields}};
struct HsState { HScore score; bool schedule; };
constinit bool g_hs_schedule = false;

void hs_tick(TickContext& ctx) {
    HsState& s = ctx.state_as<HsState>();
    s.score.total += (i32)(ctx.rng().next() & 7);
    s.score.draws += 1;
    if (g_hs_schedule && ctx.tick == 0) ctx.schedule_in(HPing{1}, 100);
}
bool hs_startup(Engine& e, ModuleInstance& self) {
    HsState* s = arena_push<HsState>(self.arena);
    if (!s) return false;
    self.set_state(s);
    return engine_publish(e, self, ".score", &s->score);
}
constinit const SystemDesc k_hs_systems[] = {
    {.name = ".tick", .phase = Phase::Sim, .fn = hs_tick, .writes = ".score", .emits = "HPing"},
};
KV_MODULE_DEFINE(k_hs) = {.name = "hs", .events = k_hs_events, .resources = k_hs_resources, .systems = k_hs_systems,
                          .startup = hs_startup, .memory = {.reserve = 64 * 1024}};

u64 run_hs(u32 ticks, u64 seed, bool schedule, u64* after_reset = nullptr) {
    const ModuleDesc* const list[] = {&k_hs};
    Engine e;
    EngineConfig cfg;
    cfg.seed = seed;
    g_hs_schedule = schedule;
    if (!engine_init(e, list, cfg)) return 0;
    for (u32 i = 0; i < ticks; ++i) engine_frame(e, e.clock.tick_dt);
    u64 h = engine_hash_state(e);
    if (after_reset) {
        engine_pause(e, true);
        engine_sim_reset(e);
        HsState* s = engine_module_state_as<HsState>(e, "hs");
        *s = HsState{};
        *after_reset = engine_hash_state(e);
    }
    engine_shutdown(e);
    return h;
}

} // namespace

void test_hash(kv::TestCtx& c) {
    using namespace kv;
    c.begin("hash");
    const ModuleDesc* const ab[] = {&k_ha, &k_hb};
    const ModuleDesc* const ba[] = {&k_hb, &k_ha};
    const ModuleDesc* const xab[] = {&k_hx, &k_ha, &k_hb};
    u64 h[4];
    u16 kinds[4];
    const Span<const ModuleDesc* const> lists[] = {ab, ba, xab, ab};
    for (u32 i = 0; i < 4; ++i) {
        Engine e;
        KV_CHECK(c, engine_init(e, lists[i]));
        h[i] = world_hash(e, i == 3 ? 0.5f : 0.0f, &kinds[i]);
        engine_shutdown(e);
    }
    KV_CHECK(c, kinds[0] != kinds[1] && kinds[1] != kinds[2]); // the ids did move
    KV_CHECK(c, h[0] == h[1]);
    KV_CHECK(c, h[1] == h[2]); // the extra module's kinds are empty: no state
    KV_CHECK(c, h[3] != h[0]); // and the state still counts

    // The stream, the published score and the wheel are sim state.
    u64 fresh = 0;
    u64 a = run_hs(5, 0, false, &fresh);
    KV_CHECK(c, a != 0 && a == run_hs(5, 0, false));  // the same run hashes the same
    KV_CHECK(c, a != run_hs(5, 1, false));            // another seed, another stream and score
    KV_CHECK(c, a != run_hs(6, 0, false));            // a draw more
    KV_CHECK(c, a != run_hs(5, 0, true));             // a timer waiting
    KV_CHECK(c, fresh == run_hs(0, 0, false));        // a fresh sim: the stream as engine_init left it
}
