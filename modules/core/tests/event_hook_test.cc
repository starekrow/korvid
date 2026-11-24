// The event hook (KR23): a dev build hands it every delivered event with its type's name,
// field table, emitter and all its bytes, a type without a field table included; null
// takes it away; ship has none.
#include "korvid/core/engine.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

struct HkNote { u32 n; f32 weight; };       // a field table
struct HkBlob { u8 bytes[64]; };            // none, and longer than the trace ring keeps
constinit const FieldDesc k_note_fields[] = {KV_FIELDS(HkNote, n, weight)};
constinit const EventTypeDesc k_hk_events[] = {KV_EVENT_EX(HkNote, k_note_fields, 0), KV_EVENT(HkBlob)};

void hk_emit(TickContext& ctx) {
    ctx.emit(HkNote{(u32)ctx.tick * 10 + 1, 0.5f});
    HkBlob b;
    for (u32 i = 0; i < sizeof b.bytes; ++i) b.bytes[i] = (u8)(i * 3 + ctx.tick);
    ctx.emit(b);
    ctx.emit(HkNote{(u32)ctx.tick * 10 + 2, 1.5f});
}
constinit const SystemDesc k_hk_systems[] = {{.name = ".emit", .phase = Phase::Sim, .fn = hk_emit, .emits = "HkNote, HkBlob"}};
KV_MODULE_DEFINE(k_hk) = {.name = "hk", .events = k_hk_events, .systems = k_hk_systems};

struct Seen {
    u32  notes = 0, blobs = 0, others = 0;
    u32  note_n[8] = {};
    bool fields_ok = true, emitter_ok = true, blob_ok = true, where_ok = true;
};

void hook(void* user, const DeliveredEvent& ev) {
    Seen& s = *(Seen*)user;
    if (strcmp(ev.type_name, "HkNote") == 0) {
        const HkNote* n = (const HkNote*)ev.data;
        if (s.notes < 8) s.note_n[s.notes] = n->n;
        ++s.notes;
        s.fields_ok = s.fields_ok && ev.fields.len == 2 && strcmp(ev.fields[0].name, "n") == 0 && ev.size == sizeof(HkNote);
    } else if (strcmp(ev.type_name, "HkBlob") == 0) {
        const HkBlob* b = (const HkBlob*)ev.data;
        bool all = ev.size == sizeof(HkBlob) && ev.fields.len == 0;
        for (u32 i = 0; all && i < sizeof b->bytes; ++i) all = b->bytes[i] == (u8)(i * 3 + ev.tick);
        s.blob_ok = s.blob_ok && all;
        ++s.blobs;
    } else {
        ++s.others;
        return;
    }
    s.emitter_ok = s.emitter_ok && strcmp(ev.emitter, "hk.emit") == 0;
    s.where_ok   = s.where_ok && strcmp(ev.where, "sim") == 0;
}

} // namespace

void test_event_hook(kv::TestCtx& c) {
    using namespace kv;
    c.begin("event hook");
    const ModuleDesc* const list[] = {&k_hk};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    Seen s;
#if KV_DEV
    KV_CHECK(c, engine_set_event_hook(e, hook, &s));
    engine_frame(e, e.clock.tick_dt);
    engine_frame(e, e.clock.tick_dt);
    KV_CHECK(c, s.notes == 4 && s.blobs == 2 && s.others == 0);
    KV_CHECK(c, s.note_n[0] == 1 && s.note_n[1] == 2 && s.note_n[2] == 11 && s.note_n[3] == 12); // emission order within a type
    KV_CHECK(c, s.fields_ok && s.blob_ok && s.emitter_ok && s.where_ok);
    // Taken away: nothing more.
    KV_CHECK(c, engine_set_event_hook(e, nullptr, &s));
    engine_frame(e, e.clock.tick_dt);
    KV_CHECK(c, s.notes == 4 && s.blobs == 2 && e.trace.hook_user == nullptr);
#else
    KV_CHECK(c, !engine_set_event_hook(e, hook, &s));
    engine_frame(e, e.clock.tick_dt);
    KV_CHECK(c, s.notes == 0 && s.blobs == 0);
#endif
    engine_shutdown(e);
}
