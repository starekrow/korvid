// An engine kind in Lua. The kind's layout stays where the engine declared it (its
// components' field tables); Lua reads and writes rows by field name, and holds whatever
// a row cannot.
//
//   local files = ...                 -- from script_push_kind
//   local h = files:spawn()           -- a handle (an integer), every field zero; nil if the kind is full
//   local h = files:spawn{ size = 3, path = "a.png" }
//   files:set(h, "size", 4)           -- an error for a stale handle or an unknown field
//   files:get(h, "size")              -- nil for a stale handle
//   local r = files:row(h)            -- a row object: r.size, r.path = "b.png", r.handle
//   for h in files:each() do ... end  -- live rows in slot order, the engine's own order
//   files:destroy(h)  files:valid(h)  files:live()  files:capacity()  files:name()  files:fields()
//
// A field is named "field" when only one of the kind's components has it, and always
// "Component.field". Values: a bool is a boolean; integers are integers (a u64 past 2^63
// wraps, as Lua's integers do); floats are numbers; a handle is its 64 bits as an integer,
// 0 for none; a char[N] is a string; a u8[N] is a string of exactly N bytes (a digest);
// any other array is a list.
#include "korvid/script/script.h"
#include "korvid/core/kind.h"
#include <string.h>

namespace kv {

namespace {

constexpr const char* kKindMeta = "kv.kind";
constexpr const char* kRowMeta  = "kv.row";

struct LuaField { const Column* column; const FieldDesc* field; };
struct LuaKind {
    Table*   table;
    u32      field_count;
    LuaField* fields() { return (LuaField*)(this + 1); }
};
struct LuaRow { LuaKind* kind; Handle handle; };

void push_scalar(lua_State* L, FieldType t, const u8* p) {
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

void push_field(lua_State* L, const FieldDesc& f, const u8* row) {
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
void set_scalar(lua_State* L, const FieldDesc& f, u8* p, int index) {
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

void set_field(lua_State* L, const FieldDesc& f, u8* row, int index) {
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

LuaKind* check_kind(lua_State* L, int i) { return (LuaKind*)luaL_checkudata(L, i, kKindMeta); }
Handle check_handle(lua_State* L, int i) { return Handle{(u64)luaL_checkinteger(L, i)}; }

// The field named by the string at `name_index`, looked up in the kind object's map (the
// user value of the object at `kind_index`). Null, with nothing left on the stack, if the
// kind has none.
const LuaField* find_field(lua_State* L, LuaKind* k, int kind_index, int name_index) {
    kind_index = lua_absindex(L, kind_index);
    name_index = lua_absindex(L, name_index);
    lua_getiuservalue(L, kind_index, 1);
    lua_pushvalue(L, name_index);
    lua_rawget(L, -2);
    lua_Integer at = lua_tointeger(L, -1);
    lua_pop(L, 2);
    return at >= 1 && at <= (lua_Integer)k->field_count ? &k->fields()[at - 1] : nullptr;
}

u8* row_of(const LuaField& f, Handle h) { return f.column->data + (usize)h.index() * f.column->stride; }

int kind_spawn(lua_State* L) {
    LuaKind* k = check_kind(L, 1);
    bool init = lua_istable(L, 2);
    Handle h = k->table->spawn();
    if (h.is_null()) { lua_pushnil(L); return 1; }
    if (init) {
        // Checked before anything is written, so a bad name leaves no half-made row.
        lua_pushnil(L);
        while (lua_next(L, 2)) {
            if (lua_type(L, -2) != LUA_TSTRING || !find_field(L, k, 1, -2)) {
                k->table->destroy(h);
                return luaL_error(L, "%s: no field '%s'", k->table->name, lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "(not a name)");
            }
            lua_pop(L, 1);
        }
        lua_pushnil(L);
        while (lua_next(L, 2)) {
            const LuaField* f = find_field(L, k, 1, -2);
            set_field(L, *f->field, row_of(*f, h), -1);
            lua_pop(L, 1);
        }
    }
    lua_pushinteger(L, (lua_Integer)h.bits);
    return 1;
}

int kind_destroy(lua_State* L) { lua_pushboolean(L, check_kind(L, 1)->table->destroy(check_handle(L, 2))); return 1; }
int kind_valid(lua_State* L) { lua_pushboolean(L, lua_isinteger(L, 2) && check_kind(L, 1)->table->valid(check_handle(L, 2))); return 1; }
int kind_live(lua_State* L) { lua_pushinteger(L, check_kind(L, 1)->table->live()); return 1; }
int kind_capacity(lua_State* L) { lua_pushinteger(L, check_kind(L, 1)->table->capacity); return 1; }
int kind_name(lua_State* L) { lua_pushstring(L, check_kind(L, 1)->table->name); return 1; }

int kind_get(lua_State* L) {
    LuaKind* k = check_kind(L, 1);
    Handle h = check_handle(L, 2);
    luaL_checktype(L, 3, LUA_TSTRING);
    const LuaField* f = find_field(L, k, 1, 3);
    if (!f) return luaL_error(L, "%s: no field '%s'", k->table->name, lua_tostring(L, 3));
    if (!k->table->valid(h)) { lua_pushnil(L); return 1; }
    push_field(L, *f->field, row_of(*f, h));
    return 1;
}

int kind_set(lua_State* L) {
    LuaKind* k = check_kind(L, 1);
    Handle h = check_handle(L, 2);
    luaL_checktype(L, 3, LUA_TSTRING);
    const LuaField* f = find_field(L, k, 1, 3);
    if (!f) return luaL_error(L, "%s: no field '%s'", k->table->name, lua_tostring(L, 3));
    if (!k->table->valid(h)) return luaL_error(L, "%s: a stale handle", k->table->name);
    set_field(L, *f->field, row_of(*f, h), 4);
    return 0;
}

int kind_each_next(lua_State* L) {
    LuaKind* k = (LuaKind*)lua_touserdata(L, lua_upvalueindex(1));
    u32 i = (u32)lua_tointeger(L, lua_upvalueindex(2));
    Table* t = k->table;
    for (; i < t->rows(); ++i) {
        if (!t->is_alive(i)) continue;
        lua_pushinteger(L, (lua_Integer)i + 1);
        lua_replace(L, lua_upvalueindex(2));
        lua_pushinteger(L, (lua_Integer)t->handle_at(i).bits);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}
int kind_each(lua_State* L) {
    check_kind(L, 1);
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 0);
    lua_pushcclosure(L, kind_each_next, 2);
    return 1;
}

int kind_fields(lua_State* L) {
    LuaKind* k = check_kind(L, 1);
    lua_createtable(L, (int)k->field_count, 0);
    for (u32 i = 0; i < k->field_count; ++i) {
        lua_pushfstring(L, "%s.%s", k->fields()[i].column->desc->name, k->fields()[i].field->name);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 1;
}

int kind_row(lua_State* L) {
    LuaKind* k = check_kind(L, 1);
    Handle h = check_handle(L, 2);
    if (!k->table->valid(h)) { lua_pushnil(L); return 1; }
    LuaRow* r = (LuaRow*)lua_newuserdatauv(L, sizeof(LuaRow), 1);
    r->kind   = k;
    r->handle = h;
    lua_pushvalue(L, 1);
    lua_setiuservalue(L, -2, 1); // the kind object lives as long as its rows do
    luaL_setmetatable(L, kRowMeta);
    return 1;
}

int row_index(lua_State* L) {
    LuaRow* r = (LuaRow*)luaL_checkudata(L, 1, kRowMeta);
    if (lua_type(L, 2) != LUA_TSTRING) { lua_pushnil(L); return 1; }
    lua_getiuservalue(L, 1, 1);
    const LuaField* f = find_field(L, r->kind, -1, 2);
    if (!f) {
        const char* key = lua_tostring(L, 2);
        if (strcmp(key, "handle") == 0) { lua_pushinteger(L, (lua_Integer)r->handle.bits); return 1; }
        if (strcmp(key, "alive") == 0) { lua_pushboolean(L, r->kind->table->valid(r->handle)); return 1; }
        return luaL_error(L, "%s: no field '%s'", r->kind->table->name, key);
    }
    if (!r->kind->table->valid(r->handle)) return luaL_error(L, "%s: the row is gone (a stale handle)", r->kind->table->name);
    push_field(L, *f->field, row_of(*f, r->handle));
    return 1;
}

int row_newindex(lua_State* L) {
    LuaRow* r = (LuaRow*)luaL_checkudata(L, 1, kRowMeta);
    luaL_checktype(L, 2, LUA_TSTRING);
    lua_getiuservalue(L, 1, 1);
    const LuaField* f = find_field(L, r->kind, -1, 2);
    if (!f) return luaL_error(L, "%s: no field '%s'", r->kind->table->name, lua_tostring(L, 2));
    if (!r->kind->table->valid(r->handle)) return luaL_error(L, "%s: the row is gone (a stale handle)", r->kind->table->name);
    set_field(L, *f->field, row_of(*f, r->handle), 3);
    return 0;
}

constexpr luaL_Reg k_kind_methods[] = {
    {"spawn", kind_spawn}, {"destroy", kind_destroy}, {"valid", kind_valid}, {"live", kind_live},
    {"capacity", kind_capacity}, {"name", kind_name}, {"get", kind_get}, {"set", kind_set},
    {"each", kind_each}, {"fields", kind_fields}, {"row", kind_row}, {nullptr, nullptr},
};

} // namespace

void script_push_kind(lua_State* L, Table* table) {
    u32 count = 0;
    for (const Column& c : table->columns) count += c.desc->fields.len;
    LuaKind* k = (LuaKind*)lua_newuserdatauv(L, sizeof(LuaKind) + sizeof(LuaField) * (count ? count : 1), 1);
    k->table       = table;
    k->field_count = count;
    int self = lua_gettop(L);
    lua_createtable(L, 0, (int)count * 2); // name -> 1-based index
    int map = lua_gettop(L);
    u32 n = 0;
    for (const Column& c : table->columns) {
        for (const FieldDesc& f : c.desc->fields) {
            k->fields()[n] = LuaField{&c, &f};
            ++n;
            lua_pushfstring(L, "%s.%s", c.desc->name, f.name);
            lua_pushinteger(L, n);
            lua_rawset(L, map);
            // The bare name, unless another component of the kind has a field so named.
            u32 named = 0;
            for (const Column& c2 : table->columns)
                for (const FieldDesc& f2 : c2.desc->fields) if (strcmp(f2.name, f.name) == 0) ++named;
            if (named == 1) { lua_pushinteger(L, n); lua_setfield(L, map, f.name); }
        }
    }
    lua_setiuservalue(L, self, 1);
    if (luaL_newmetatable(L, kKindMeta)) {
        lua_newtable(L);
        luaL_setfuncs(L, k_kind_methods, 0);
        lua_setfield(L, -2, "__index");
    }
    lua_setmetatable(L, self);
    if (luaL_newmetatable(L, kRowMeta)) {
        lua_pushcfunction(L, row_index);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, row_newindex);
        lua_setfield(L, -2, "__newindex");
    }
    lua_pop(L, 1);
}

} // namespace kv
