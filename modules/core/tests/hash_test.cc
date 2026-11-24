// The state hash: the same world hashes the same however its ktables were numbered. Two
// engines with the modules in opposite orders, and a third with an extra module ahead of
// them, give their ktables different ids; a handle in a field still hashes the same.
#include "korvid/core/engine.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

struct HPos { f32 x, y; };
struct HRef { Handle other; u32 n; };

constinit const FieldDesc k_pos_fields[] = {KV_FIELDS(HPos, x, y)};
constinit const FieldDesc k_ref_fields[] = {KV_FIELDS(HRef, other, n)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(HPos, k_pos_fields), KV_COMPONENT(HRef, k_ref_fields)};

constinit const KTableDesc k_a_ktables[] = {{.name = "ha.thing", .components = "HPos, HRef", .capacity = 8}};
constinit const KTableDesc k_b_ktables[] = {{.name = "hb.mark", .components = "HPos", .capacity = 8}};
constinit const KTableDesc k_x_ktables[] = {{.name = "hx.extra", .components = "HPos", .capacity = 8}, {.name = "hx.more", .components = "HPos", .capacity = 8}};
KV_MODULE_DEFINE(k_ha) = {.name = "ha", .components = k_components, .ktables = k_a_ktables};
KV_MODULE_DEFINE(k_hb) = {.name = "hb", .ktables = k_b_ktables};
KV_MODULE_DEFINE(k_hx) = {.name = "hx", .ktables = k_x_ktables};

// The same world in any engine: two marks, a thing pointing at the second.
u64 world_hash(Engine& e, f32 nudge, u16* mark_kind) {
    KTable* marks = engine_ktable(e, "hb.mark");
    KTable* things = engine_ktable(e, "ha.thing");
    marks->spawn(HPos{1, 2});
    Handle m = marks->spawn(HPos{3, 4});
    things->spawn(HPos{5 + nudge, 6}, HRef{m, 7});
    *mark_kind = marks->handle_kind;
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

// A module before hs with an event type of its own, which moves hs's event ids.
struct HPad { u32 n; };
constinit const EventTypeDesc k_hpad_events[] = {KV_EVENT(HPad)};
KV_MODULE_DEFINE(k_hpad) = {.name = "hpad", .events = k_hpad_events};

u64 run_hs(u32 ticks, u64 seed, bool schedule, u64* after_reset = nullptr, bool padded = false) {
    const ModuleDesc* const plain[] = {&k_hs};
    const ModuleDesc* const pad[] = {&k_hpad, &k_hs};
    Span<const ModuleDesc* const> list = padded ? Span<const ModuleDesc* const>(pad) : Span<const ModuleDesc* const>(plain);
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

// A cosmetic resource with a field table (KR17): described, never hashed, and its object
// need not be plain data.
struct HTransport {
    f64 position = 0;
    u32 state    = 0;
    HTransport() = default;
    HTransport(const HTransport& o) : position(o.position), state(o.state) {} // not trivially copyable
    HTransport& operator=(const HTransport& o) { position = o.position; state = o.state; return *this; }
};
constinit const FieldDesc k_transport_fields[] = {KV_FIELDS(HTransport, position, state)};
constinit const ResourceDesc k_ht_resources[] = {{.name = ".transport", .fields = k_transport_fields, .cosmetic = true}};
HTransport g_transport;
bool ht_startup(Engine& e, ModuleInstance& self) { return engine_publish(e, self, ".transport", &g_transport); }
KV_MODULE_DEFINE(k_ht) = {.name = "ht", .resources = k_ht_resources, .startup = ht_startup};
constinit const ResourceDesc k_hbad_resources[] = {{.name = ".loose", .cosmetic = true}};
KV_MODULE_DEFINE(k_hbad) = {.name = "hbad", .resources = k_hbad_resources};

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
    KV_CHECK(c, h[1] == h[2]); // the extra module's ktables are empty: no state
    KV_CHECK(c, h[3] != h[0]); // and the state still counts

    // The stream, the published score and the wheel are sim state.
    u64 fresh = 0;
    u64 a = run_hs(5, 0, false, &fresh);
    KV_CHECK(c, a != 0 && a == run_hs(5, 0, false));  // the same run hashes the same
    KV_CHECK(c, a != run_hs(5, 1, false));            // another seed, another stream and score
    KV_CHECK(c, a != run_hs(6, 0, false));            // a draw more
    KV_CHECK(c, a != run_hs(5, 0, true));             // a timer waiting
    // An event type registered before hs's moves its ids, not the hash: a timer waiting is
    // hashed by its type's name.
    KV_CHECK(c, run_hs(5, 0, true) == run_hs(5, 0, true, nullptr, true));
    KV_CHECK(c, fresh == run_hs(0, 0, false));        // a fresh sim: the stream as engine_init left it

    // A cosmetic resource: published though not plain, read by its fields, not hashed.
    {
        const ModuleDesc* const list[] = {&k_ht};
        Engine e;
        g_transport.position = 0;
        g_transport.state    = 0;
        KV_CHECK(c, engine_init(e, list));
        u32 id = engine_resource_id(e, "ht.transport");
        KV_CHECK(c, id && e.resources[id - 1].ptr == &g_transport && e.resources[id - 1].fields.len == 2 && !e.resources[id - 1].hashed);
        u64 before = engine_hash_state(e);
        g_transport.position = 12.25;
        g_transport.state    = 3;
        KV_CHECK(c, engine_hash_state(e) == before);
        char line[128];
        schema_describe("transport", e.resources[id - 1].fields, e.resources[id - 1].ptr, line, sizeof line);
        KV_CHECK(c, strstr(line, "position=12.25") && strstr(line, "state=3"));
        engine_shutdown(e);
    }
    {
        const ModuleDesc* const list[] = {&k_hbad};
        Engine e;
        KV_CHECK(c, !engine_init(e, list)); // cosmetic with no field table: refused
        engine_shutdown(e);
    }
}
