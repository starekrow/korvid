#include "korvid/save/save.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace kv {

namespace {

constexpr u32 kAlive = Pool<u8>::ALIVE_BIT;

void scalar_out(SaveWriter& w, FieldType t, const u8* p) {
    switch (t) {
    case FieldType::Bool: w.bool_value(*p != 0); return;
    case FieldType::U8:   w.uint_value(*p); return;
    case FieldType::U16:  { u16 v; memcpy(&v, p, 2); w.uint_value(v); return; }
    case FieldType::U32:  { u32 v; memcpy(&v, p, 4); w.uint_value(v); return; }
    case FieldType::U64:  { u64 v; memcpy(&v, p, 8); w.uint_value(v); return; }
    case FieldType::I8:   w.int_value(*(const i8*)p); return;
    case FieldType::I16:  { i16 v; memcpy(&v, p, 2); w.int_value(v); return; }
    case FieldType::I32:  { i32 v; memcpy(&v, p, 4); w.int_value(v); return; }
    case FieldType::I64:  { i64 v; memcpy(&v, p, 8); w.int_value(v); return; }
    case FieldType::F32:  { f32 v; memcpy(&v, p, 4); w.f32_value(v); return; }
    case FieldType::F64:  { f64 v; memcpy(&v, p, 8); w.f64_value(v); return; }
    case FieldType::Handle: { Handle h; memcpy(&h, p, 8); save_handle(w, h); return; }
    case FieldType::Char: case FieldType::Count: break;
    }
    w.null_value();
}

// A float from a number, or from the strings the writer uses for what JSON cannot say.
bool float_of(const JsonValue& v, f64* out) {
    if (v.is_number()) { *out = v.number; return true; }
    if (!v.is_string()) return false;
    if (!strcmp(v.str, "nan"))  { *out = (f64)NAN; return true; }
    if (!strcmp(v.str, "inf"))  { *out = (f64)INFINITY; return true; }
    if (!strcmp(v.str, "-inf")) { *out = -(f64)INFINITY; return true; }
    return false;
}

template <class T>
void store(u8* p, T v) { memcpy(p, &v, sizeof v); }

// Integers clamp to the field's range: a save from a build with a wider field still loads.
bool scalar_in(FieldType t, u8* p, const JsonValue& v, const SaveRemap& remap) {
    switch (t) {
    case FieldType::Bool:
        if (!v.is_bool() && !v.is_number()) return false;
        *p = v.number != 0.0;
        return true;
    case FieldType::U8: case FieldType::U16: case FieldType::U32: case FieldType::U64: {
        if (!v.is_number()) return false;
        u64 x = json_u64(&v, 0);
        switch (t) {
        case FieldType::U8:  store<u8>(p, (u8)(x > 0xFFu ? 0xFFu : x)); break;
        case FieldType::U16: store<u16>(p, (u16)(x > 0xFFFFu ? 0xFFFFu : x)); break;
        case FieldType::U32: store<u32>(p, (u32)(x > 0xFFFFFFFFu ? 0xFFFFFFFFu : x)); break;
        default:             store<u64>(p, x); break;
        }
        return true;
    }
    case FieldType::I8: case FieldType::I16: case FieldType::I32: case FieldType::I64: {
        if (!v.is_number()) return false;
        i64 x = json_i64(&v, 0);
        auto clamp = [&](i64 lo, i64 hi) { return x < lo ? lo : x > hi ? hi : x; };
        switch (t) {
        case FieldType::I8:  store<i8>(p, (i8)clamp(-128, 127)); break;
        case FieldType::I16: store<i16>(p, (i16)clamp(-32768, 32767)); break;
        case FieldType::I32: store<i32>(p, (i32)clamp(-2147483648ll, 2147483647ll)); break;
        default:             store<i64>(p, x); break;
        }
        return true;
    }
    case FieldType::F32: { f64 x; if (!float_of(v, &x)) return false; store<f32>(p, (f32)x); return true; }
    case FieldType::F64: { f64 x; if (!float_of(v, &x)) return false; store<f64>(p, x); return true; }
    case FieldType::Handle:
        if (!v.is_null() && !v.is_string()) return false;
        store<Handle>(p, load_handle(&v, remap));
        return true;
    case FieldType::Char: case FieldType::Count: break;
    }
    return false;
}

void warn_shape(const FieldDesc& f, const JsonValue& v) {
    KV_WARN("save", "field '%s': a %s where %s %s belongs; skipped", f.name, json_type_name(v.type),
            f.count > 1 && f.type != FieldType::Char ? "an array of" : "a", field_type_name(f.type));
}

} // namespace

// ---- handles ----------------------------------------------------------------------------------

void save_handle(SaveWriter& w, Handle h) {
    if (h.is_null()) { w.null_value(); return; }
    const char* kind = kv_handle_kind_name(h.kind());
    if (!kind[0]) {
        KV_WARN("save", "a handle of unknown kind %u written as null", h.kind());
        w.null_value();
        return;
    }
    char text[64];
    int n = snprintf(text, sizeof text, "%s#%u:%u", kind, h.index(), h.gen());
    w.string_value(text, (u32)(n < (int)sizeof text ? n : (int)sizeof text - 1));
}

bool parse_handle(const char* text, SavedHandle* out, char* kind, u32 cap) {
    if (!text || !cap) return false;
    const char* hash = strrchr(text, '#');
    if (!hash || hash == text || (u32)(hash - text) >= cap) return false;
    u64 index = 0, gen = 0;
    const char* p = hash + 1;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') { index = index * 10 + (u64)(*p++ - '0'); if (index > 0xFFFFFFFFull) return false; }
    if (*p++ != ':' || *p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') { gen = gen * 10 + (u64)(*p++ - '0'); if (gen > KV_HANDLE_MAX_GEN) return false; }
    if (*p) return false;
    memcpy(kind, text, (usize)(hash - text));
    kind[hash - text] = 0;
    out->kind  = kind;
    out->index = (u32)index;
    out->gen   = (u32)gen;
    return true;
}

Handle load_handle(const JsonValue* v, const SaveRemap& remap) {
    if (!v || v->is_null()) return Handle{};
    char kind[KV_KIND_NAME_MAX];
    SavedHandle saved;
    if (!v->is_string() || !parse_handle(v->str, &saved, kind, sizeof kind)) {
        KV_WARN("save", "'%s' is not a handle; read as null", v->is_string() ? v->str : json_type_name(v->type));
        return Handle{};
    }
    u16 id = kv_handle_kind_find(kind);
    Handle remade = id ? Handle::make(id, saved.gen, saved.index) : Handle{};
    if (remap.fn) return remap.fn(saved, remade, remap.user);
    if (!id) KV_WARN("save", "no kind '%s' now; '%s' read as null", kind, v->str);
    return remade;
}

// ---- fields -----------------------------------------------------------------------------------

void save_fields(SaveWriter& w, Span<const FieldDesc> fields, const void* row) {
    const u8* base = (const u8*)row;
    w.begin_object();
    for (const FieldDesc& f : fields) {
        w.key(f.name);
        const u8* p = base + f.offset;
        if (f.type == FieldType::Char) {
            u32 n = 0;
            while (n < f.count && p[n]) ++n;
            w.string_value((const char*)p, n);
        } else if (f.count == 1) {
            scalar_out(w, f.type, p);
        } else {
            u32 size = field_type_size(f.type);
            w.begin_array(true);
            for (u32 i = 0; i < f.count; ++i) scalar_out(w, f.type, p + i * size);
            w.end_array();
        }
    }
    w.end_object();
}

u32 load_fields(const JsonDoc& doc, const JsonValue& obj, Span<const FieldDesc> fields, void* row, const SaveRemap& remap) {
    if (!obj.is_object()) return 0;
    u8* base = (u8*)row;
    u32 read = 0;
    for (const FieldDesc& f : fields) {
        const JsonValue* v = json_get(doc, obj, f.name);
        if (!v) continue;
        u8* p = base + f.offset;
        bool ok = true;
        if (f.type == FieldType::Char) {
            ok = v->is_string() && schema_field_set_string(f, row, v->str);
        } else if (f.count == 1) {
            ok = scalar_in(f.type, p, *v, remap);
        } else if (!v->is_array()) {
            ok = false;
        } else {
            // Lanes the file lacks keep their values; lanes past the field's are ignored.
            u32 size = field_type_size(f.type), lane = 0;
            for (const JsonValue* e = json_first(doc, *v); e && lane < f.count && ok; e = json_next(doc, *e), ++lane)
                ok = scalar_in(f.type, p + lane * size, *e, remap);
        }
        if (ok) ++read;
        else warn_shape(f, *v);
    }
    return read;
}

// ---- kinds ------------------------------------------------------------------------------------

void save_kind(SaveWriter& w, const Table& t) {
    w.begin_object();
    w.key("free");
    w.begin_array();
    t.slots.each_free([&](u32 i) {
        w.begin_array(true);
        w.uint_value(i);
        w.uint_value(t.handle_at(i).gen());
        w.end_array();
    });
    w.end_array();
    w.key("rows");
    w.begin_array();
    for (u32 i = 0; i < t.rows(); ++i) {
        if (!t.is_alive(i)) continue;
        w.begin_object();
        w.key("slot"); w.uint_value(i);
        w.key("gen");  w.uint_value(t.handle_at(i).gen());
        for (const Column& c : t.columns) {
            w.key(c.desc->name);
            save_fields(w, c.desc->fields, c.data + (usize)i * c.stride);
        }
        w.end_object();
    }
    w.end_array();
    w.end_object();
}

bool load_kind(const JsonDoc& doc, const JsonValue& v, Table& t, const SaveRemap& remap) {
    auto fail = [&](const char* why) {
        KV_ERROR("save", "kind '%s': %s", t.name, why);
        t.restore_slots(Span<const u32>(), Span<const u32>());
        return false;
    };
    if (!v.is_object()) return fail("not an object");
    const JsonValue* rows = json_get(doc, v, "rows");
    const JsonValue* free_list = json_get(doc, v, "free");
    if ((rows && !rows->is_array()) || (free_list && !free_list->is_array())) return fail("'rows' and 'free' are arrays");

    // The slots first: how many, which are live with what generation, the free order.
    u32 n = 0;
    auto slot_of = [](const JsonValue* s) { return s && s->is_number() && s->number >= 0.0 ? json_u64(s, 0) : ~0ull; };
    for (const JsonValue* r = rows ? json_first(doc, *rows) : nullptr; r; r = json_next(doc, *r)) {
        u64 s = slot_of(json_get(doc, *r, "slot"));
        if (s >= t.desc->capacity) return fail("a row's slot is missing or past the capacity");
        if (s + 1 > n) n = (u32)s + 1;
    }
    for (const JsonValue* f = free_list ? json_first(doc, *free_list) : nullptr; f; f = json_next(doc, *f)) {
        u64 s = f->is_array() ? slot_of(json_at(doc, *f, 0)) : ~0ull;
        if (s >= t.desc->capacity) return fail("a free slot is malformed or past the capacity");
        if (s + 1 > n) n = (u32)s + 1;
    }
    Array<u32> gens, order;
    gens.alloc = order.alloc = mem_general();
    gens.resize_fill(n, 1u); // a slot the file never names: dead, generation 1, reused last
    bool ok = true;
    for (const JsonValue* r = rows ? json_first(doc, *rows) : nullptr; r && ok; r = json_next(doc, *r)) {
        u32 s = (u32)slot_of(json_get(doc, *r, "slot"));
        u64 g = json_u64(json_get(doc, *r, "gen"), 0);
        if (gens[s] & kAlive || g == 0 || g > KV_HANDLE_MAX_GEN) ok = false;
        else gens[s] = (u32)g | kAlive;
    }
    for (const JsonValue* f = free_list ? json_first(doc, *free_list) : nullptr; f && ok; f = json_next(doc, *f)) {
        u32 s = (u32)slot_of(json_at(doc, *f, 0));
        u64 g = json_u64(json_at(doc, *f, 1), 0);
        if (gens[s] & kAlive || g == 0 || g > KV_HANDLE_MAX_GEN) ok = false;
        else { gens[s] = (u32)g; order.push(s); }
    }
    ok = ok && t.restore_slots(gens.span(), order.span());
    gens.free();
    order.free();
    if (!ok) return fail("a slot is named twice or has a bad generation");

    // Then the rows, component by component; a component the file lacks stays zero.
    for (const JsonValue* r = rows ? json_first(doc, *rows) : nullptr; r; r = json_next(doc, *r)) {
        u32 s = (u32)slot_of(json_get(doc, *r, "slot"));
        for (Column& c : t.columns) {
            u8* row = c.data + (usize)s * c.stride;
            if (const JsonValue* obj = json_get(doc, *r, c.desc->name)) {
                if (obj->is_object()) load_fields(doc, *obj, c.desc->fields, row, remap);
                else KV_WARN("save", "kind '%s': %s is a %s, not an object; left zero", t.name, c.desc->name, json_type_name(obj->type));
            }
            if (c.prev) memcpy(c.prev + (usize)s * c.stride, row, c.stride);
        }
    }
    return true;
}

void save_kinds(SaveWriter& w, Engine& e, Span<const char* const> kinds) {
    w.begin_object();
    for (const char* name : kinds) {
        const Table* t = engine_table(e, name);
        if (!t) { KV_WARN("save", "no kind '%s' to save", name); continue; }
        w.key(t->name);
        save_kind(w, *t);
    }
    w.end_object();
}

u32 load_kinds(const JsonDoc& doc, const JsonValue& obj, Engine& e, const SaveRemap& remap) {
    if (!obj.is_object()) return 0;
    u32 loaded = 0;
    for (const JsonValue* k = json_first(doc, obj); k; k = json_next(doc, *k)) {
        Table* t = engine_table(e, k->key);
        if (!t) { KV_WARN("save", "the save has a kind '%s' this build does not; skipped", k->key); continue; }
        if (load_kind(doc, *k, *t, remap)) ++loaded;
    }
    return loaded;
}

} // namespace kv
