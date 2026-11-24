// The field mapping every binding shares (fields.h).
#include "fields.h"
#include <string.h>

namespace kv {

static void push_scalar(lua_State* L, FieldType t, const u8* p) {
    switch (t) {
    case FieldType::Bool:   lua_pushboolean(L, *p != 0); return;
    case FieldType::U8:     lua_pushinteger(L, *p); return;
    case FieldType::I8:     lua_pushinteger(L, *(const i8*)p); return;
    case FieldType::Char:   lua_pushinteger(L, *(const char*)p); return;
    case FieldType::U16:    { u16 v; memcpy(&v, p, sizeof v); lua_pushinteger(L, v); return; }
    case FieldType::I16:    { i16 v; memcpy(&v, p, sizeof v); lua_pushinteger(L, v); return; }
    case FieldType::U32:    { u32 v; memcpy(&v, p, sizeof v); lua_pushinteger(L, v); return; }
    case FieldType::I32:    { i32 v; memcpy(&v, p, sizeof v); lua_pushinteger(L, v); return; }
    case FieldType::U64:    { u64 v; memcpy(&v, p, sizeof v); lua_pushinteger(L, (lua_Integer)v); return; }
    case FieldType::I64:    { i64 v; memcpy(&v, p, sizeof v); lua_pushinteger(L, v); return; }
    case FieldType::Handle: { u64 v; memcpy(&v, p, sizeof v); lua_pushinteger(L, (lua_Integer)v); return; }
    case FieldType::F32:    { f32 v; memcpy(&v, p, sizeof v); lua_pushnumber(L, v); return; }
    case FieldType::F64:    { f64 v; memcpy(&v, p, sizeof v); lua_pushnumber(L, v); return; }
    case FieldType::Count:  break;
    }
    lua_pushnil(L);
}

void script_push_field(lua_State* L, const FieldDesc& f, const u8* row) {
    const u8* p = row + f.offset;
    if (f.type == FieldType::Char) {
        usize n = 0;
        while (n < f.count && p[n]) ++n;
        lua_pushlstring(L, (const char*)p, n);
        return;
    }
    if (f.count == 1) { push_scalar(L, f.type, p); return; }
    if (f.type == FieldType::U8) { lua_pushlstring(L, (const char*)p, f.count); return; }
    u32 size = field_type_size(f.type);
    lua_createtable(L, f.count, 0);
    for (u32 i = 0; i < f.count; ++i) {
        push_scalar(L, f.type, p + (usize)i * size);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
}

// The value at `index` into one scalar. Raises for the wrong type or an integer the
// field cannot hold: a row that silently holds something else is the worse outcome.
static void set_scalar(lua_State* L, const FieldDesc& f, u8* p, int index) {
    auto integer = [&](lua_Integer lo, lua_Integer hi) -> lua_Integer {
        int ok = 0;
        lua_Integer v = lua_tointegerx(L, index, &ok);
        if (!ok) luaL_error(L, "field '%s': an integer expected, got %s", f.name, luaL_typename(L, index));
        if (v < lo || v > hi) luaL_error(L, "field '%s': %I does not fit a %s", f.name, (LUAI_UACINT)v, field_type_name(f.type));
        return v;
    };
    constexpr lua_Integer kMin = LUA_MININTEGER, kMax = LUA_MAXINTEGER;
    switch (f.type) {
    case FieldType::Bool:   *p = lua_toboolean(L, index) ? 1 : 0; return;
    case FieldType::U8:     *p = (u8)integer(0, 0xFF); return;
    case FieldType::I8:     *(i8*)p = (i8)integer(-128, 127); return;
    case FieldType::Char:   *(char*)p = (char)integer(-128, 255); return;
    case FieldType::U16:    { u16 v = (u16)integer(0, 0xFFFF); memcpy(p, &v, sizeof v); return; }
    case FieldType::I16:    { i16 v = (i16)integer(-32768, 32767); memcpy(p, &v, sizeof v); return; }
    case FieldType::U32:    { u32 v = (u32)integer(0, 0xFFFFFFFFll); memcpy(p, &v, sizeof v); return; }
    case FieldType::I32:    { i32 v = (i32)integer(-2147483647ll - 1, 2147483647ll); memcpy(p, &v, sizeof v); return; }
    case FieldType::U64:
    case FieldType::Handle: { u64 v = (u64)integer(kMin, kMax); memcpy(p, &v, sizeof v); return; }
    case FieldType::I64:    { i64 v = (i64)integer(kMin, kMax); memcpy(p, &v, sizeof v); return; }
    case FieldType::F32:
    case FieldType::F64: {
        int ok = 0;
        lua_Number v = lua_tonumberx(L, index, &ok);
        if (!ok) luaL_error(L, "field '%s': a number expected, got %s", f.name, luaL_typename(L, index));
        if (f.type == FieldType::F32) { f32 x = (f32)v; memcpy(p, &x, sizeof x); }
        else memcpy(p, &v, sizeof v);
        return;
    }
    case FieldType::Count: break;
    }
}

void script_set_field(lua_State* L, const FieldDesc& f, u8* row, int index) {
    index = lua_absindex(L, index);
    u8* p = row + f.offset;
    if (f.type == FieldType::Char) {
        usize n = 0;
        // The type first: lua_tolstring would turn a number into a string where it lies.
        const char* s = lua_type(L, index) == LUA_TSTRING ? lua_tolstring(L, index, &n) : nullptr;
        if (!s) luaL_error(L, "field '%s': a string expected, got %s", f.name, luaL_typename(L, index));
        if (n >= f.count || memchr(s, 0, n)) luaL_error(L, "field '%s': a string of %d bytes does not fit char[%d]", f.name, (int)n, (int)f.count);
        memcpy(p, s, n);
        memset(p + n, 0, f.count - n);
        return;
    }
    if (f.count == 1) { set_scalar(L, f, p, index); return; }
    if (f.type == FieldType::U8) {
        usize n = 0;
        const char* s = lua_type(L, index) == LUA_TSTRING ? lua_tolstring(L, index, &n) : nullptr;
        if (!s || n != f.count) luaL_error(L, "field '%s': a string of exactly %d bytes expected", f.name, (int)f.count);
        memcpy(p, s, n);
        return;
    }
    if (!lua_istable(L, index) || luaL_len(L, index) != (lua_Integer)f.count) luaL_error(L, "field '%s': a list of %d expected", f.name, (int)f.count);
    u32 size = field_type_size(f.type);
    for (u32 i = 0; i < f.count; ++i) {
        lua_rawgeti(L, index, (lua_Integer)i + 1);
        set_scalar(L, f, p + (usize)i * size, -1);
        lua_pop(L, 1);
    }
}

const FieldDesc* script_find_field(Span<const FieldDesc> fields, const char* name) {
    for (const FieldDesc& f : fields) if (strcmp(f.name, name) == 0) return &f;
    return nullptr;
}

void script_push_fields(lua_State* L, Span<const FieldDesc> fields, const u8* row) {
    lua_createtable(L, 0, (int)fields.len);
    for (const FieldDesc& f : fields) {
        script_push_field(L, f, row);
        lua_setfield(L, -2, f.name);
    }
}

void script_set_fields(lua_State* L, Span<const FieldDesc> fields, u8* row, int index, const char* what) {
    index = lua_absindex(L, index);
    luaL_checktype(L, index, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "%s: a field name expected, got %s", what, luaL_typename(L, -2));
        const char* name = lua_tostring(L, -2);
        const FieldDesc* f = script_find_field(fields, name);
        if (!f) luaL_error(L, "%s has no field '%s'", what, name);
        script_set_field(L, *f, row, -1);
        lua_pop(L, 1);
    }
}

} // namespace kv
