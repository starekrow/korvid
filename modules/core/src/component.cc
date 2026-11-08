#include "korvid/core/component.h"
#include "korvid/core/hash.h"
#include "korvid/core/utf8.h"
#include <stdio.h>
#include <string.h>

namespace kv {

u32 field_type_size(FieldType t) {
    switch (t) {
    case FieldType::Bool: case FieldType::U8: case FieldType::I8: case FieldType::Char: return 1;
    case FieldType::U16:  case FieldType::I16:                      return 2;
    case FieldType::U32:  case FieldType::I32: case FieldType::F32: return 4;
    case FieldType::U64:  case FieldType::I64: case FieldType::F64: case FieldType::Handle: return 8;
    case FieldType::Count: break;
    }
    return 0;
}

const char* field_type_name(FieldType t) {
    static const char* const names[] = {"bool", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f32", "f64", "handle", "char"};
    return (u32)t < KV_COUNTOF(names) ? names[(u32)t] : "?";
}

const char* schema_check(u32 total, Span<const FieldDesc> fields, const char** field) {
    *field = nullptr;
    for (u32 i = 0; i < fields.len; ++i) {
        const FieldDesc& f = fields[i];
        *field = f.name;
        if (!f.name || !f.name[0]) { *field = nullptr; return "has an unnamed field"; }
        u32 size = field_type_size(f.type);
        if (size == 0) return "has an unknown type";
        if (f.count == 0) return "has zero count";
        if ((usize)f.offset + (usize)size * f.count > total) return "extends past the end of the struct";
        if (f.offset % size) return "is misaligned for its type";
        for (u32 j = 0; j < i; ++j) {
            if (strcmp(fields[j].name, f.name) == 0) return "is named twice";
            u32 lo = fields[j].offset, hi = lo + field_type_size(fields[j].type) * fields[j].count;
            if (f.offset < hi && lo < f.offset + size * f.count) return "overlaps an earlier field";
        }
    }
    *field = nullptr;
    return nullptr;
}

const FieldDesc* schema_field(Span<const FieldDesc> fields, const char* name) {
    for (const FieldDesc& f : fields)
        if (strcmp(f.name, name) == 0) return &f;
    return nullptr;
}

bool schema_has_handles(Span<const FieldDesc> fields) {
    for (const FieldDesc& f : fields) if (f.type == FieldType::Handle) return true;
    return false;
}

bool schema_field_as_f64(const FieldDesc& f, const void* row, f64* out) {
    const u8* p = (const u8*)row + f.offset;
    switch (f.type) {
    case FieldType::Bool: case FieldType::U8: *out = *p; return true;
    case FieldType::I8:  *out = *(const i8*)p; return true;
    case FieldType::U16: { u16 v; memcpy(&v, p, 2); *out = v; return true; }
    case FieldType::I16: { i16 v; memcpy(&v, p, 2); *out = v; return true; }
    case FieldType::U32: { u32 v; memcpy(&v, p, 4); *out = v; return true; }
    case FieldType::I32: { i32 v; memcpy(&v, p, 4); *out = v; return true; }
    case FieldType::F32: { f32 v; memcpy(&v, p, 4); *out = v; return true; }
    case FieldType::U64: { u64 v; memcpy(&v, p, 8); *out = (f64)v; return true; }
    case FieldType::I64: { i64 v; memcpy(&v, p, 8); *out = (f64)v; return true; }
    case FieldType::F64: { f64 v; memcpy(&v, p, 8); *out = v; return true; }
    case FieldType::Handle: case FieldType::Char: case FieldType::Count: break;
    }
    return false;
}

bool schema_field_lane(const FieldDesc& f, const void* row, u32 lane, f64* out) {
    if (lane >= f.count || f.type == FieldType::Handle) return false;
    FieldDesc one = f;
    one.count  = 1;
    one.offset = (u16)(f.offset + lane * field_type_size(f.type));
    return schema_field_as_f64(one, row, out);
}

bool schema_field_set(const FieldDesc& f, void* row, u32 lane, f64 value) {
    if (lane >= f.count || f.type == FieldType::Handle) return false;
    u8* p = (u8*)row + f.offset + lane * field_type_size(f.type);
    i64 i = value >= 0 ? (i64)(value + 0.5) : -(i64)(-value + 0.5);
    switch (f.type) {
    case FieldType::Bool: *p = value != 0.0; return true;
    case FieldType::U8:   *p = (u8)i; return true;
    case FieldType::I8:   *(i8*)p = (i8)i; return true;
    case FieldType::U16: { u16 v = (u16)i; memcpy(p, &v, 2); return true; }
    case FieldType::I16: { i16 v = (i16)i; memcpy(p, &v, 2); return true; }
    case FieldType::U32: { u32 v = (u32)i; memcpy(p, &v, 4); return true; }
    case FieldType::I32: { i32 v = (i32)i; memcpy(p, &v, 4); return true; }
    case FieldType::F32: { f32 v = (f32)value; memcpy(p, &v, 4); return true; }
    case FieldType::U64: { u64 v = (u64)i; memcpy(p, &v, 8); return true; }
    case FieldType::I64: { i64 v = i; memcpy(p, &v, 8); return true; }
    case FieldType::F64: memcpy(p, &value, 8); return true;
    case FieldType::Handle: case FieldType::Char: case FieldType::Count: break;
    }
    return false;
}

namespace {

// Every scalar as 64 bits: integers widened, floats as their bit pattern.
u64 scalar_bits(FieldType t, const u8* p) {
    switch (t) {
    case FieldType::Bool: case FieldType::U8: case FieldType::I8: case FieldType::Char:       return *p;
    case FieldType::U16:  case FieldType::I16:                      { u16 v; memcpy(&v, p, 2); return v; }
    case FieldType::U32:  case FieldType::I32: case FieldType::F32: { u32 v; memcpy(&v, p, 4); return v; }
    case FieldType::U64:  case FieldType::I64: case FieldType::F64: case FieldType::Handle: { u64 v; memcpy(&v, p, 8); return v; }
    case FieldType::Count: break;
    }
    return 0;
}

int print_scalar(FieldType t, const u8* p, char* out, usize cap) {
    switch (t) {
    case FieldType::Bool:   return snprintf(out, cap, "%s", *p ? "true" : "false");
    case FieldType::U8:     return snprintf(out, cap, "%u", *p);
    case FieldType::I8:     return snprintf(out, cap, "%d", *(const i8*)p);
    case FieldType::U16:    { u16 v; memcpy(&v, p, 2); return snprintf(out, cap, "%u", v); }
    case FieldType::I16:    { i16 v; memcpy(&v, p, 2); return snprintf(out, cap, "%d", v); }
    case FieldType::U32:    { u32 v; memcpy(&v, p, 4); return snprintf(out, cap, "%u", v); }
    case FieldType::I32:    { i32 v; memcpy(&v, p, 4); return snprintf(out, cap, "%d", v); }
    case FieldType::F32:    { f32 v; memcpy(&v, p, 4); return snprintf(out, cap, "%g", (double)v); }
    case FieldType::U64:    { u64 v; memcpy(&v, p, 8); return snprintf(out, cap, "%llu", (unsigned long long)v); }
    case FieldType::I64:    { i64 v; memcpy(&v, p, 8); return snprintf(out, cap, "%lld", (long long)v); }
    case FieldType::F64:    { f64 v; memcpy(&v, p, 8); return snprintf(out, cap, "%g", v); }
    case FieldType::Handle: {
        Handle h; memcpy(&h.bits, p, 8);
        if (h.is_null()) return snprintf(out, cap, "null");
        const char* kind = kv_handle_kind_name(h.kind());
        return snprintf(out, cap, "%s#%u", kind[0] ? kind : "?", h.index());
    }
    case FieldType::Char: return snprintf(out, cap, "%u", *p); // strings print whole, not here
    case FieldType::Count: break;
    }
    return snprintf(out, cap, "?");
}

// snprintf-style accumulation: `at` keeps growing past cap so the return value is right.
struct Out {
    char* buf; usize cap; usize at = 0;
    void put(const char* s) { int n = snprintf(buf + (at < cap ? at : cap), at < cap ? cap - at : 0, "%s", s); if (n > 0) at += (usize)n; }
    void scalar(FieldType t, const u8* p) { int n = print_scalar(t, p, buf + (at < cap ? at : cap), at < cap ? cap - at : 0); if (n > 0) at += (usize)n; }
};

} // namespace

u64 schema_hash(Span<const FieldDesc> fields, const void* row) {
    const u8* base = (const u8*)row;
    u64 h = 0x9e3779b97f4a7c15ull ^ fields.len;
    for (const FieldDesc& f : fields) {
        u32 size = field_type_size(f.type);
        for (u32 i = 0; i < f.count; ++i)
            h = hash_mix(h ^ scalar_bits(f.type, base + f.offset + i * size));
    }
    return hash_mix(h);
}

bool schema_equal(Span<const FieldDesc> fields, const void* a, const void* b) {
    for (const FieldDesc& f : fields) {
        usize bytes = (usize)field_type_size(f.type) * f.count;
        if (memcmp((const u8*)a + f.offset, (const u8*)b + f.offset, bytes) != 0) return false;
    }
    return true;
}

int schema_describe(const char* name, Span<const FieldDesc> fields, const void* row, char* out, usize cap) {
    if (cap) out[0] = 0;
    Out o{out, cap};
    o.put(name);
    o.put("{");
    const u8* base = (const u8*)row;
    for (u32 i = 0; i < fields.len; ++i) {
        const FieldDesc& f = fields[i];
        if (i) o.put(", ");
        o.put(f.name);
        o.put("=");
        if (f.type == FieldType::Char) {
            int n = schema_field_string(f, row, o.buf + (o.at < o.cap ? o.at : o.cap), o.at < o.cap ? o.cap - o.at : 0);
            if (n > 0) o.at += (usize)n;
            continue;
        }
        u32 size = field_type_size(f.type);
        if (f.count > 1) o.put("(");
        for (u32 k = 0; k < f.count; ++k) {
            if (k) o.put(", ");
            o.scalar(f.type, base + f.offset + k * size);
        }
        if (f.count > 1) o.put(")");
    }
    o.put("}");
    return (int)o.at;
}

int schema_field_string(const FieldDesc& f, const void* row, char* out, usize cap) {
    if (cap) out[0] = 0;
    if (f.type != FieldType::Char) return snprintf(out, cap, "?");
    const u8* p = (const u8*)row + f.offset;
    usize at = 0;
    auto put = [&](const char* s, usize n) {
        for (usize i = 0; i < n; ++i, ++at)
            if (at + 1 < cap) out[at] = s[i];
    };
    put("\"", 1);
    for (u32 i = 0; i < f.count && p[i]; ++i) {
        char c = (char)p[i];
        if (c == '"' || c == '\\') { char e[2] = {'\\', c}; put(e, 2); }
        else if ((u8)c < 0x20 || c == 0x7F) { char e[5]; snprintf(e, sizeof e, "\\x%02x", (u8)c); put(e, 4); }
        else put(&c, 1);
    }
    put("\"", 1);
    if (cap) out[at < cap ? at : cap - 1] = 0;
    // A string cut by the buffer must not end in a cut sequence.
    if (at >= cap && cap) utf8_trim(out);
    return (int)at;
}

bool schema_field_set_string(const FieldDesc& f, void* row, const char* str) {
    if (f.type != FieldType::Char) return false;
    utf8_copy((char*)row + f.offset, f.count, str);
    return true;
}

bool schema_each_handle(Span<const FieldDesc> fields, void* row, bool (*fn)(Handle&, void*), void* user) {
    for (const FieldDesc& f : fields) {
        if (f.type != FieldType::Handle) continue;
        Handle* h = (Handle*)((u8*)row + f.offset); // offset is 8-aligned: schema_check said so
        for (u32 i = 0; i < f.count; ++i)
            if (!fn(h[i], user)) return false;
    }
    return true;
}

} // namespace kv
