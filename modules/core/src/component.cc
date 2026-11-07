#include "korvid/core/component.h"
#include "korvid/core/hash.h"
#include <stdio.h>
#include <string.h>

namespace kv {

u32 field_type_size(FieldType t) {
    switch (t) {
    case FieldType::Bool: case FieldType::U8: case FieldType::I8:   return 1;
    case FieldType::U16:  case FieldType::I16:                      return 2;
    case FieldType::U32:  case FieldType::I32: case FieldType::F32: return 4;
    case FieldType::U64:  case FieldType::I64: case FieldType::F64: case FieldType::Handle: return 8;
    case FieldType::Count: break;
    }
    return 0;
}

const char* field_type_name(FieldType t) {
    static const char* const names[] = {"bool", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f32", "f64", "handle"};
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
    case FieldType::Handle: case FieldType::Count: break;
    }
    return false;
}

namespace {

// Every scalar as 64 bits: integers widened, floats as their bit pattern.
u64 scalar_bits(FieldType t, const u8* p) {
    switch (t) {
    case FieldType::Bool: case FieldType::U8: case FieldType::I8:                             return *p;
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
