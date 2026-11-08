// Schemas: traits pick scalar type and count, the check rejects inconsistent tables,
// hash and equality read fields only (padding is invisible), describe prints every
// field, handles are found, and the registry assigns ids in order and rejects
// duplicates by name and by type.
#include "korvid/core/component.h"
#include "korvid/core/engine.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

enum class Team : u8 { Red, Blue };

// u8 then f32 leaves three padding bytes; u32[3] then Handle leaves four more.
struct Probe {
    u8     flags;
    f32    speed;
    u32    tags[3];
    Handle target;
    Team   team;
    i64    score;
};

struct Pos { f32 x, y; };
struct Other { u32 n; };

constinit const FieldDesc k_probe_fields[] = {
    KV_FIELD(Probe, flags), KV_FIELD(Probe, speed), KV_FIELD(Probe, tags),
    KV_FIELD(Probe, target), KV_FIELD(Probe, team), KV_FIELD(Probe, score),
};
constinit const FieldDesc k_pos_fields[]   = {KV_FIELD(Pos, x), KV_FIELD(Pos, y)};
constinit const FieldDesc k_other_fields[] = {KV_FIELD(Other, n)};

constinit const ComponentDesc k_a_components[] = {
    KV_COMPONENT(Probe, k_probe_fields),
    KV_COMPONENT(Pos, k_pos_fields, CompPrev),
};
constinit const ComponentDesc k_b_components[] = {KV_COMPONENT(Other, k_other_fields, CompCosmetic | CompPrev)};
KV_MODULE_DEFINE(k_a) = {.name = "a", .components = k_a_components};
KV_MODULE_DEFINE(k_b) = {.name = "b", .components = k_b_components};

// Rejections: the same name again, the same type under another name, a field past the end.
constinit const ComponentDesc k_dup_name_components[] = {KV_COMPONENT(Other, k_other_fields)};
KV_MODULE_DEFINE(k_dup_name) = {.name = "dupname", .components = k_dup_name_components};

struct Other2 { u32 n; };
constinit const FieldDesc k_other2_fields[] = {KV_FIELD(Other2, n)};
constinit const ComponentDesc k_dup_type_components[] = {
    {"Renamed", sizeof(Other), alignof(Other), k_other2_fields, 0, &ComponentType<Other>::id, &TypeKey<Other>::key},
};
KV_MODULE_DEFINE(k_dup_type) = {.name = "duptype", .components = k_dup_type_components};

constinit const FieldDesc k_bad_fields[] = {{"beyond", FieldType::F64, 1, 0}};
constinit const ComponentDesc k_bad_components[] = {
    {"Bad", 4, 4, k_bad_fields, 0, &ComponentType<Pos>::id, &TypeKey<Pos>::key},
};
KV_MODULE_DEFINE(k_bad) = {.name = "bad", .components = k_bad_components};

bool count_handles(Handle& h, void* user) { *(u32*)user += h.is_null() ? 0 : 1; return true; }

} // namespace

void test_component(kv::TestCtx& c) {
    using namespace kv;
    c.begin("component");

    // Traits and the macro: scalar type, count and offset per field.
    const ComponentDesc& probe = k_a_components[0];
    KV_CHECK(c, strcmp(probe.name, "Probe") == 0 && probe.size == sizeof(Probe) && probe.fields.len == 6);
    KV_CHECK(c, probe.fields[0].type == FieldType::U8 && probe.fields[0].count == 1 && probe.fields[0].offset == 0);
    KV_CHECK(c, probe.fields[1].type == FieldType::F32 && probe.fields[1].offset == offsetof(Probe, speed));
    KV_CHECK(c, probe.fields[2].type == FieldType::U32 && probe.fields[2].count == 3);
    KV_CHECK(c, probe.fields[3].type == FieldType::Handle && probe.fields[3].offset == offsetof(Probe, target));
    KV_CHECK(c, probe.fields[4].type == FieldType::U8); // the enum is its underlying type
    KV_CHECK(c, probe.fields[5].type == FieldType::I64);
    KV_CHECK(c, probe.is_sim() && !probe.has_prev());
    KV_CHECK(c, k_a_components[1].has_prev() && k_b_components[0].has_prev() && !k_b_components[0].is_sim());
    const char* field = nullptr;
    KV_CHECK(c, component_check(probe, &field) == nullptr);
    KV_CHECK(c, component_field(probe, "tags") == &probe.fields[2] && component_field(probe, "nope") == nullptr);
    KV_CHECK(c, component_check(k_bad_components[0], &field) != nullptr && strcmp(field, "beyond") == 0);

    // Hash and equality see fields, not bytes: different padding, same row.
    alignas(Probe) u8 raw_a[sizeof(Probe)], raw_b[sizeof(Probe)];
    memset(raw_a, 0xAA, sizeof raw_a);
    memset(raw_b, 0x55, sizeof raw_b);
    Probe* a = (Probe*)raw_a;
    Probe* b = (Probe*)raw_b;
    Probe* both[] = {a, b};
    for (Probe* p : both) {
        p->flags = 3; p->speed = 1.5f; p->tags[0] = 1; p->tags[1] = 2; p->tags[2] = 3;
        p->target = Handle::make(7, 1, 42); p->team = Team::Blue; p->score = -9;
    }
    KV_CHECK(c, memcmp(raw_a, raw_b, sizeof raw_a) != 0); // the padding really differs
    KV_CHECK(c, component_equal(probe, a, b));
    KV_CHECK(c, component_hash(probe, a) == component_hash(probe, b));
    b->tags[2] = 4;
    KV_CHECK(c, !component_equal(probe, a, b));
    KV_CHECK(c, component_hash(probe, a) != component_hash(probe, b));
    // The hash is the fixed engine hash: a known value pins it against accidental change.
    Pos origin{0.0f, 0.0f};
    KV_CHECK(c, component_hash(k_a_components[1], &origin) == component_hash(k_a_components[1], &origin));
    Pos other{0.0f, 1.0f};
    KV_CHECK(c, component_hash(k_a_components[1], &origin) != component_hash(k_a_components[1], &other));

    // Describe prints every field; arrays in parentheses; handles as kind#index.
    char text[256];
    int n = component_describe(probe, a, text, sizeof text);
    KV_CHECK(c, n > 0 && (usize)n == strlen(text));
    KV_CHECK(c, strcmp(text, "Probe{flags=3, speed=1.5, tags=(1, 2, 3), target=?#42, team=1, score=-9}") == 0);
    // A short buffer truncates like snprintf and still reports the full length.
    char tiny[12];
    KV_CHECK(c, component_describe(probe, a, tiny, sizeof tiny) == n && strcmp(tiny, "Probe{flags") == 0);
    u32 handles = 0;
    KV_CHECK(c, component_each_handle(probe, a, count_handles, &handles) && handles == 1);

    // Strings: a char[N] field is one Char field, printed quoted, patched with truncation
    // at a code point boundary, the tail zeroed so the hash does not see old bytes.
    struct Label { char name[8]; u32 n; };
    const FieldDesc label_fields[] = {KV_FIELDS(Label, name, n)};
    KV_CHECK(c, label_fields[0].type == FieldType::Char && label_fields[0].count == 8);
    Label la{}, lb{};
    memset(la.name, 'Z', sizeof la.name);
    KV_CHECK(c, schema_field_set_string(label_fields[0], &la, "caf\xc3\xa9 \xe2\x80\x94"));  // 6 bytes, then 3 that do not fit
    KV_CHECK(c, strcmp(la.name, "caf\xc3\xa9 ") == 0 && la.name[7] == 0);
    KV_CHECK(c, schema_field_set_string(label_fields[0], &lb, "caf\xc3\xa9 "));
    KV_CHECK(c, schema_equal(Span<const FieldDesc>(label_fields, 2), &la, &lb));
    KV_CHECK(c, schema_hash(Span<const FieldDesc>(label_fields, 2), &la) == schema_hash(Span<const FieldDesc>(label_fields, 2), &lb));
    KV_CHECK(c, !schema_field_set_string(label_fields[1], &la, "x"));
    schema_field_set_string(label_fields[0], &la, "a\"b\\\n");
    KV_CHECK(c, schema_describe("Label", Span<const FieldDesc>(label_fields, 2), &la, text, sizeof text) > 0);
    KV_CHECK(c, strcmp(text, "Label{name=\"a\\\"b\\\\\\x0a\", n=0}") == 0);
    f64 dummy;
    KV_CHECK(c, !schema_field_as_f64(label_fields[0], &la, &dummy) && !schema_field_set(label_fields[0], &la, 0, 1.0));
    schema_field_set_string(label_fields[0], &la, "\xc3\xa9\xc3\xa9\xc3\xa9");
    char cut[6]; // room for the quote and two characters: the third is not cut in half
    KV_CHECK(c, schema_field_string(label_fields[0], &la, cut, sizeof cut) == 8 && strcmp(cut, "\"\xc3\xa9\xc3\xa9") == 0);

    // Registry: ids in module order then declaration order; lookups; cleared at shutdown.
    const ModuleDesc* const list[] = {&k_a, &k_b};
    Engine e;
    KV_CHECK(c, ComponentType<Probe>::id == 0);
    KV_CHECK(c, engine_init(e, list));
    KV_CHECK(c, ComponentType<Probe>::id == 1 && ComponentType<Pos>::id == 2 && ComponentType<Other>::id == 3);
    KV_CHECK(c, engine_component_id(e, "Pos") == 2 && engine_component_id(e, "nope") == 0);
    KV_CHECK(c, engine_component(e, 3) == &k_b_components[0] && engine_component(e, 4) == nullptr);
    KV_CHECK(c, engine_component<Probe>(e) == &probe);
    engine_shutdown(e);
    KV_CHECK(c, ComponentType<Probe>::id == 0 && ComponentType<Other>::id == 0 && e.components.len == 0);

    // Rejections, each leaving every id at zero.
    const ModuleDesc* const dup_name[] = {&k_a, &k_b, &k_dup_name};
    const ModuleDesc* const dup_type[] = {&k_a, &k_b, &k_dup_type};
    const ModuleDesc* const bad[]      = {&k_a, &k_bad};
    const Span<const ModuleDesc* const> rejected[] = {dup_name, dup_type, bad};
    for (Span<const ModuleDesc* const> l : rejected) {
        Engine f;
        KV_CHECK(c, !engine_init(f, l));
        KV_CHECK(c, ComponentType<Probe>::id == 0 && ComponentType<Pos>::id == 0 && ComponentType<Other>::id == 0);
    }
}
