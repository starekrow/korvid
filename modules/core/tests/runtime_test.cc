// Event types registered while the engine runs (KR19): from a field table built at
// runtime, emitted from outside the schedule, delivered to listeners at the boundary,
// described and traced by name, never hashed, and taken away with their listeners. And
// listening to a declared type, which any code outside the schedule may do.
#include "korvid/core/engine.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

struct RtPing { u32 n; };
constinit const FieldDesc k_ping_fields[] = {KV_FIELDS(RtPing, n)};
constinit const EventTypeDesc k_rt_events[] = {KV_EVENT_EX(RtPing, k_ping_fields, 0)};
u32 g_ping_n = 0;
void rt_tick(TickContext& ctx) { ctx.emit(RtPing{++g_ping_n}); }
constinit const SystemDesc k_rt_systems[] = {{.name = ".tick", .phase = Phase::Sim, .fn = rt_tick, .emits = "RtPing"}};
KV_MODULE_DEFINE(k_rt) = {.name = "rt", .events = k_rt_events, .systems = k_rt_systems};

// What a script module would build at runtime: a line of lyrics, its layout made here.
struct Heard { u32 count = 0; u32 last_n = 0; f64 last_at = 0; char last_text[24] = {}; u32 pings = 0; };
void on_line(void* user, u32, const void* ev) {
    Heard& h = *(Heard*)user;
    ++h.count;
    memcpy(&h.last_n, (const u8*)ev, 4);
    memcpy(&h.last_at, (const u8*)ev + 8, 8);
    memcpy(h.last_text, (const u8*)ev + 16, sizeof h.last_text);
}
void on_ping(void* user, u32, const void* ev) { Heard& h = *(Heard*)user; ++h.pings; h.last_n = ((const RtPing*)ev)->n; }
struct Traced { u32 lines = 0; bool named = false; };
[[maybe_unused]] void trace_hook(void* user, const DeliveredEvent& ev) {
    Traced& t = *(Traced*)user;
    if (!strcmp(ev.type_name, "lyrics.Line")) { ++t.lines; t.named = ev.fields.len == 3 && !strcmp(ev.fields[2].name, "text"); }
}

} // namespace

void test_runtime_types(kv::TestCtx& c) {
    using namespace kv;
    c.begin("runtime event types");
    const ModuleDesc* const list[] = {&k_rt};
    // A tick of the same sim with nothing added, for its hash.
    u64 plain_hash = 0;
    {
        Engine e0;
        KV_CHECK(c, engine_init(e0, list));
        engine_frame(e0, e0.clock.tick_dt);
        plain_hash = engine_hash_state(e0);
        engine_shutdown(e0);
        g_ping_n = 0;
    }
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    // The table, made at runtime: { u32 n; f64 at; char text[24] }, 40 bytes.
    FieldDesc fields[3] = {{"n", FieldType::U32, 1, 0}, {"at", FieldType::F64, 1, 8}, {"text", FieldType::Char, 24, 16}};
    EventTypeDesc d{};
    d.name = "lyrics.Line";
    d.size = 40;
    d.align = 8;
    d.fields = Span<const FieldDesc>(fields, 3);
    u32 before_types = e.event_types.len;
    u32 id = engine_add_event_type(e, "rt", d);
    KV_CHECK(c, id == before_types && engine_event_type_id(e, "lyrics.Line") == id);
    KV_CHECK(c, engine_add_event_type(e, "rt", d) == 0);   // the name taken
    EventTypeDesc bad = d;
    bad.name = "lyrics.Bad";
    bad.size = 20;                                          // a field past the end
    KV_CHECK(c, engine_add_event_type(e, "rt", bad) == 0);
    bad.size = 40;
    KV_CHECK(c, engine_add_event_type(e, "nobody", bad) == 0); // no such module

    Heard h;
    Traced tr;
    KV_CHECK(c, engine_listen(e, id, on_line, &h));
#if KV_DEV
    engine_set_event_hook(e, trace_hook, &tr);
#endif
    u64 hash0 = engine_hash_state(e);
    u8 ev[40] = {};
    u32 n = 7;
    f64 at = 12.5;
    memcpy(ev, &n, 4);
    memcpy(ev + 8, &at, 8);
    memcpy(ev + 16, "the night is young", 19);
    KV_CHECK(c, engine_emit(e, id, ev));
    KV_CHECK(c, h.count == 0); // at the next boundary
    engine_frame(e, e.clock.tick_dt);
    KV_CHECK(c, h.count == 1 && h.last_n == 7 && h.last_at == 12.5 && !strcmp(h.last_text, "the night is young"));
    char text[200];
    engine_describe_event(e, id, ev, text, sizeof text);
    KV_CHECK(c, strstr(text, "lyrics.Line{") && strstr(text, "n=7"));
#if KV_DEV
    KV_CHECK(c, tr.lines == 1 && tr.named);
#endif
    // Not sim state: the hash is a run's without it.
    KV_CHECK(c, engine_hash_state(e) == plain_hash && hash0 != engine_hash_state(e));
    // A listener on a declared type: every one the sim emits, from now on.
    KV_CHECK(c, engine_listen(e, engine_event_type_id(e, "RtPing"), on_ping, &h));
    engine_frame(e, e.clock.tick_dt);
    engine_frame(e, e.clock.tick_dt);
    KV_CHECK(c, h.pings == 2 && h.last_n == g_ping_n);
    engine_unlisten(e, engine_event_type_id(e, "RtPing"), on_ping, &h);
    engine_frame(e, e.clock.tick_dt);
    KV_CHECK(c, h.pings == 2);

    // Taken away: what was pending goes, its listeners with it; the id and the name are
    // not given out again.
    KV_CHECK(c, engine_emit(e, id, ev));
    KV_CHECK(c, engine_remove_event_type(e, id) && !engine_remove_event_type(e, id));
    engine_frame(e, e.clock.tick_dt);
    KV_CHECK(c, h.count == 1 && !engine_emit(e, id, ev) && engine_event_type_id(e, "lyrics.Line") == 0 && e.listeners.len == 0);
    KV_CHECK(c, engine_add_event_type(e, "rt", d) == 0);
    EventTypeDesc next = d;
    next.name = "lyrics.Word";
    u32 id2 = engine_add_event_type(e, nullptr, next);
    KV_CHECK(c, id2 > id && !engine_remove_event_type(e, engine_event_type_id(e, "RtPing"))); // a declared type stays
#if KV_DEV
    engine_set_event_hook(e, nullptr, nullptr);
#endif
    engine_shutdown(e);
}
