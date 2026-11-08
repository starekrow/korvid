// Fields and kinds: every field type through a save and back to the same bits; files
// that lack fields, have extra ones or the wrong shapes; a kind restored slot for slot,
// so the handles into it, the next spawns and the state hash are what they were, in an
// engine whose kinds have other ids; handles remapped by the game.
#include "korvid/save/save.h"
#include "korvid/core/engine.h"
#include "korvid/core/module.h"
#include "korvid/core/arena.h"
#include "korvid/core/test.h"
#include <math.h>
#include <string.h>

namespace {

using namespace kv;

struct Every {
    bool   b;
    u8     u8v;
    u16    u16v;
    u32    u32v;
    u64    u64v;
    i8     i8v;
    i16    i16v;
    i32    i32v;
    i64    i64v;
    f32    f32v[3];
    f64    f64v;
    Handle h;
    char   name[12];
};
constinit const FieldDesc k_every_fields[] = {KV_FIELDS(Every, b, u8v, u16v, u32v, u64v, i8v, i16v, i32v, i64v, f32v, f64v, h, name)};

struct SPos { f32 x, y; };
struct SLink { Handle to; u32 n; };
constinit const FieldDesc k_pos_fields[] = {KV_FIELDS(SPos, x, y)};
constinit const FieldDesc k_link_fields[] = {KV_FIELDS(SLink, to, n)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(SPos, k_pos_fields, CompPrev), KV_COMPONENT(SLink, k_link_fields)};
constinit const KindDesc k_kinds[] = {
    {.name = "sv.node", .components = "SPos, SLink", .capacity = 16},
    {.name = "sv.anchor", .components = "SPos", .capacity = 4},
};
constinit const KindDesc k_extra_kinds[] = {{.name = "sx.first", .components = "SPos", .capacity = 4}};
KV_MODULE_DEFINE(k_sv) = {.name = "sv", .components = k_components, .kinds = k_kinds};
KV_MODULE_DEFINE(k_sx) = {.name = "sx", .kinds = k_extra_kinds};

bool same_bits(const Every& a, const Every& b) { return schema_equal(Span<const FieldDesc>(k_every_fields), &a, &b); }

bool parse(SaveWriter& w, JsonDoc& doc) {
    return w.finish() && save_parse(SaveFormat::Json, w.out.data, w.out.len, mem_scratch(), doc);
}

Handle to_anchor(const SavedHandle& saved, Handle remade, void* user) {
    return !strcmp(saved.kind, "sv.anchor") ? *(Handle*)user : remade;
}

} // namespace

void test_fields(kv::TestCtx& c) {
    using namespace kv;
    c.begin("fields");
    const ModuleDesc* const list[] = {&k_sv};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    Handle node = engine_table(e, "sv.node")->spawn();

    Every a{};
    a.b = true; a.u8v = 250; a.u16v = 65000; a.u32v = 4000000000u; a.u64v = 18446744073709551615ull;
    a.i8v = -128; a.i16v = -32000; a.i32v = -2000000000; a.i64v = (i64)0x8000000000000000ull;
    a.f32v[0] = 0.1f; a.f32v[1] = NAN; a.f32v[2] = -INFINITY; a.f64v = 1.0 / 3.0; a.h = node;
    strcpy(a.name, "caf\xc3\xa9 \"x\"");
    SaveWriter w;
    w.init(mem_general());
    save_fields(w, k_every_fields, &a);
    JsonDoc doc;
    KV_CHECK(c, parse(w, doc));
    Every b{};
    KV_CHECK(c, load_fields(doc, doc.root(), k_every_fields, &b) == KV_COUNTOF(k_every_fields));
    KV_CHECK(c, same_bits(a, b) && b.h == node && isnan(b.f32v[1]));

    // A file from another version: fields missing keep their values, unknown keys are
    // ignored, a value of the wrong shape is skipped, integers clamp to the field.
    const char* old = "{\"u8v\": 300, \"i8v\": -1000, \"gone\": 5, \"f32v\": [7, 8], \"name\": 3, \"h\": null, \"b\": 0}";
    KV_CHECK(c, json_parse(old, strlen(old), mem_scratch(), doc));
    Every d = a;
    KV_CHECK(c, load_fields(doc, doc.root(), k_every_fields, &d) == 5); // u8v, i8v, f32v, h, b; not name
    KV_CHECK(c, d.u8v == 255 && d.i8v == -128 && d.f32v[0] == 7 && d.f32v[1] == 8 && d.f32v[2] == -INFINITY);
    KV_CHECK(c, d.h.is_null() && !d.b && !strcmp(d.name, a.name) && d.u64v == a.u64v);

    // Handles: by kind name; a kind nobody has reads as null; text that is not a handle too.
    SavedHandle sh;
    char kind[48];
    KV_CHECK(c, parse_handle("sv.node#3:7", &sh, kind, sizeof kind) && !strcmp(sh.kind, "sv.node") && sh.index == 3 && sh.gen == 7);
    KV_CHECK(c, !parse_handle("sv.node#3", &sh, kind, sizeof kind) && !parse_handle("#1:1", &sh, kind, sizeof kind) && !parse_handle("a#1:1x", &sh, kind, sizeof kind));
    const char* hs = "[\"sv.node#0:1\", \"nobody.here#0:1\", \"sv.node\", null]";
    KV_CHECK(c, json_parse(hs, strlen(hs), mem_scratch(), doc));
    KV_CHECK(c, load_handle(json_at(doc, doc.root(), 0)) == node);
    KV_CHECK(c, load_handle(json_at(doc, doc.root(), 1)).is_null() && load_handle(json_at(doc, doc.root(), 2)).is_null());
    KV_CHECK(c, load_handle(json_at(doc, doc.root(), 3)).is_null() && load_handle(nullptr).is_null());
    w.free();
    engine_shutdown(e);
}

void test_kinds(kv::TestCtx& c) {
    using namespace kv;
    c.begin("kinds");
    const ModuleDesc* const list[] = {&k_sv};
    const ModuleDesc* const other[] = {&k_sx, &k_sv}; // another kind first: every id moves

    // A world with history: slots freed in an order, a link into the other kind and
    // one inside its own.
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    Table* nodes = engine_table(e, "sv.node");
    Table* anchors = engine_table(e, "sv.anchor");
    Handle anchor = anchors->spawn(SPos{100, 200});
    Handle n[6];
    for (u32 i = 0; i < 6; ++i) n[i] = nodes->spawn(SPos{(f32)i, (f32)i * 0.5f}, SLink{anchor, i});
    nodes->destroy(n[4]);
    nodes->destroy(n[1]);
    nodes->destroy(n[3]);
    n[1] = nodes->spawn(SPos{-1, -1}, SLink{n[0], 77}); // slot 4 again, generation 2
    nodes->get<SLink>(n[5])->to = n[1];
    u64 hash_before = engine_hash_state(e);

    SaveWriter w;
    w.init(mem_general());
    const char* const kinds[] = {"sv.node", "sv.anchor"};
    save_kinds(w, e, kinds);
    KV_CHECK(c, w.finish());
    // What the next two spawns would have been.
    Handle next0 = nodes->spawn(), next1 = nodes->spawn();
    engine_shutdown(e);

    // A new engine, other ids, the same world back.
    Engine f;
    KV_CHECK(c, engine_init(f, other));
    nodes = engine_table(f, "sv.node");
    anchors = engine_table(f, "sv.anchor");
    KV_CHECK(c, nodes->kind != next0.kind());
    nodes->spawn(SPos{9, 9}); // something to replace
    JsonDoc doc;
    KV_CHECK(c, save_parse(SaveFormat::Json, w.out.data, w.out.len, mem_scratch(), doc));
    KV_CHECK(c, load_kinds(doc, doc.root(), f) == 2);
    KV_CHECK(c, engine_hash_state(f) == hash_before);
    KV_CHECK(c, nodes->live() == 4 && anchors->live() == 1 && nodes->rows() == 6);
    Handle m1 = Handle::make(nodes->kind, n[1].gen(), n[1].index());
    Handle m5 = Handle::make(nodes->kind, n[5].gen(), n[5].index());
    KV_CHECK(c, nodes->valid(m1) && nodes->get<SLink>(m1)->n == 77 && nodes->get<SPos>(m1)->x == -1);
    KV_CHECK(c, nodes->get<SLink>(m5)->to == m1);
    KV_CHECK(c, anchors->valid(nodes->get<SLink>(Handle::make(nodes->kind, 1, 0))->to));
    KV_CHECK(c, nodes->prev<SPos>(m1)->x == -1); // prev equals the loaded row
    Handle again0 = nodes->spawn(), again1 = nodes->spawn();
    KV_CHECK(c, again0.index() == next0.index() && again0.gen() == next0.gen() && again1.index() == next1.index() && again1.gen() == next1.gen());

    // A remap: the anchor is made again elsewhere; links to it follow.
    Handle fresh = anchors->spawn(SPos{1, 1});
    SaveRemap remap;
    remap.fn = to_anchor;
    remap.user = &fresh;
    KV_CHECK(c, load_kind(doc, *json_get(doc, doc.root(), "sv.node"), *nodes, remap));
    KV_CHECK(c, nodes->get<SLink>(Handle::make(nodes->kind, 1, 0))->to == fresh);
    KV_CHECK(c, nodes->get<SLink>(m5)->to == m1); // links within the kind are untouched

    // Bad files leave the kind empty and say so: a slot twice, a slot past the capacity.
    const char* twice = "{\"rows\": [{\"slot\": 1, \"gen\": 1}], \"free\": [[1, 2]]}";
    KV_CHECK(c, json_parse(twice, strlen(twice), mem_scratch(), doc) && !load_kind(doc, doc.root(), *nodes) && nodes->live() == 0);
    const char* past = "{\"rows\": [{\"slot\": 16, \"gen\": 1}]}";
    KV_CHECK(c, json_parse(past, strlen(past), mem_scratch(), doc) && !load_kind(doc, doc.root(), *nodes) && nodes->live() == 0);
    KV_CHECK(c, !nodes->spawn().is_null()); // and usable after
    // Slots the file never names are dead and reused last.
    const char* gap = "{\"rows\": [{\"slot\": 2, \"gen\": 5, \"SPos\": {\"x\": 3}}], \"free\": [[1, 4]]}";
    KV_CHECK(c, json_parse(gap, strlen(gap), mem_scratch(), doc) && load_kind(doc, doc.root(), *nodes));
    KV_CHECK(c, nodes->live() == 1 && nodes->rows() == 3 && nodes->get<SPos>(Handle::make(nodes->kind, 5, 2))->x == 3);
    Handle g1 = nodes->spawn(), g0 = nodes->spawn();
    KV_CHECK(c, g1.index() == 1 && g1.gen() == 4 && g0.index() == 0 && g0.gen() == 1);
    w.free();
    engine_shutdown(f);
}
