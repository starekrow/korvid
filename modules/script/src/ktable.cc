// An engine ktable in Lua, as a ktable object. Its layout stays where the engine declared it
// (its components' field tables); Lua reads and writes rows by field name, and holds whatever
// a row cannot.
//
//   local files = ...                 -- from script_push_ktable
//   local h = files:spawn()           -- a handle (an integer), every field zero; nil if the ktable is full
//   local h = files:spawn{ size = 3, path = "a.png" }
//   files:set(h, "size", 4)           -- an error for a stale handle or an unknown field
//   files:get(h, "size")              -- nil for a stale handle
//   local r = files:row(h)            -- a row object: r.size, r.path = "b.png", r.handle
//   for h in files:each() do ... end  -- live rows in slot order, the engine's own order
//   files:destroy(h)  files:valid(h)  files:live()  files:capacity()  files:name()  files:fields()
//
// A field is named "field" when only one of the ktable's components has it, and always
// "Component.field". Values: a bool is a boolean; integers are integers (a u64 past 2^63
// wraps, as Lua's integers do); floats are numbers; a handle is its 64 bits as an integer,
// 0 for none; a char[N] is a string; a u8[N] is a string of exactly N bytes (a digest);
// any other array is a list.
#include "korvid/script/script.h"
#include "korvid/core/ktable.h"
#include "fields.h"
#include <string.h>

namespace kv {

namespace {

constexpr const char* kKTableMeta = "kv.ktable";
constexpr const char* kRowMeta    = "kv.row";

struct LuaField { const Column* column; const FieldDesc* field; };
struct ScriptKTable {
    KTable*   table;
    u32       field_count;
    LuaField* fields() { return (LuaField*)(this + 1); }
};
struct LuaRow { ScriptKTable* ktable; Handle handle; };

ScriptKTable* check_ktable(lua_State* L, int i) { return (ScriptKTable*)luaL_checkudata(L, i, kKTableMeta); }
Handle check_handle(lua_State* L, int i) { return Handle{(u64)luaL_checkinteger(L, i)}; }

// The field named by the string at `name_index`, looked up in the ktable object's map (the
// user value of the object at `ktable_index`). Null, with nothing left on the stack, if the
// ktable has none.
const LuaField* find_field(lua_State* L, ScriptKTable* k, int ktable_index, int name_index) {
    ktable_index = lua_absindex(L, ktable_index);
    name_index = lua_absindex(L, name_index);
    lua_getiuservalue(L, ktable_index, 1);
    lua_pushvalue(L, name_index);
    lua_rawget(L, -2);
    lua_Integer at = lua_tointeger(L, -1);
    lua_pop(L, 2);
    return at >= 1 && at <= (lua_Integer)k->field_count ? &k->fields()[at - 1] : nullptr;
}

u8* row_of(const LuaField& f, Handle h) { return f.column->data + (usize)h.index() * f.column->stride; }

int ktable_spawn(lua_State* L) {
    ScriptKTable* k = check_ktable(L, 1);
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
            script_set_field(L, *f->field, row_of(*f, h), -1);
            lua_pop(L, 1);
        }
    }
    lua_pushinteger(L, (lua_Integer)h.bits);
    return 1;
}

int ktable_destroy(lua_State* L) { lua_pushboolean(L, check_ktable(L, 1)->table->destroy(check_handle(L, 2))); return 1; }
int ktable_valid(lua_State* L) { lua_pushboolean(L, lua_isinteger(L, 2) && check_ktable(L, 1)->table->valid(check_handle(L, 2))); return 1; }
int ktable_live(lua_State* L) { lua_pushinteger(L, check_ktable(L, 1)->table->live()); return 1; }
int ktable_capacity(lua_State* L) { lua_pushinteger(L, check_ktable(L, 1)->table->capacity); return 1; }
int ktable_name(lua_State* L) { lua_pushstring(L, check_ktable(L, 1)->table->name); return 1; }

int ktable_get(lua_State* L) {
    ScriptKTable* k = check_ktable(L, 1);
    Handle h = check_handle(L, 2);
    luaL_checktype(L, 3, LUA_TSTRING);
    const LuaField* f = find_field(L, k, 1, 3);
    if (!f) return luaL_error(L, "%s: no field '%s'", k->table->name, lua_tostring(L, 3));
    if (!k->table->valid(h)) { lua_pushnil(L); return 1; }
    script_push_field(L, *f->field, row_of(*f, h));
    return 1;
}

int ktable_set(lua_State* L) {
    ScriptKTable* k = check_ktable(L, 1);
    Handle h = check_handle(L, 2);
    luaL_checktype(L, 3, LUA_TSTRING);
    const LuaField* f = find_field(L, k, 1, 3);
    if (!f) return luaL_error(L, "%s: no field '%s'", k->table->name, lua_tostring(L, 3));
    if (!k->table->valid(h)) return luaL_error(L, "%s: a stale handle", k->table->name);
    script_set_field(L, *f->field, row_of(*f, h), 4);
    return 0;
}

int ktable_each_next(lua_State* L) {
    ScriptKTable* k = (ScriptKTable*)lua_touserdata(L, lua_upvalueindex(1));
    u32 i = (u32)lua_tointeger(L, lua_upvalueindex(2));
    KTable* t = k->table;
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
int ktable_each(lua_State* L) {
    check_ktable(L, 1);
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 0);
    lua_pushcclosure(L, ktable_each_next, 2);
    return 1;
}

int ktable_fields(lua_State* L) {
    ScriptKTable* k = check_ktable(L, 1);
    lua_createtable(L, (int)k->field_count, 0);
    for (u32 i = 0; i < k->field_count; ++i) {
        lua_pushfstring(L, "%s.%s", k->fields()[i].column->desc->name, k->fields()[i].field->name);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 1;
}

int ktable_row(lua_State* L) {
    ScriptKTable* k = check_ktable(L, 1);
    Handle h = check_handle(L, 2);
    if (!k->table->valid(h)) { lua_pushnil(L); return 1; }
    LuaRow* r = (LuaRow*)lua_newuserdatauv(L, sizeof(LuaRow), 1);
    r->ktable = k;
    r->handle = h;
    lua_pushvalue(L, 1);
    lua_setiuservalue(L, -2, 1); // the ktable object lives as long as its rows do
    luaL_setmetatable(L, kRowMeta);
    return 1;
}

int row_index(lua_State* L) {
    LuaRow* r = (LuaRow*)luaL_checkudata(L, 1, kRowMeta);
    if (lua_type(L, 2) != LUA_TSTRING) { lua_pushnil(L); return 1; }
    lua_getiuservalue(L, 1, 1);
    const LuaField* f = find_field(L, r->ktable, -1, 2);
    if (!f) {
        const char* key = lua_tostring(L, 2);
        if (strcmp(key, "handle") == 0) { lua_pushinteger(L, (lua_Integer)r->handle.bits); return 1; }
        if (strcmp(key, "alive") == 0) { lua_pushboolean(L, r->ktable->table->valid(r->handle)); return 1; }
        return luaL_error(L, "%s: no field '%s'", r->ktable->table->name, key);
    }
    if (!r->ktable->table->valid(r->handle)) return luaL_error(L, "%s: the row is gone (a stale handle)", r->ktable->table->name);
    script_push_field(L, *f->field, row_of(*f, r->handle));
    return 1;
}

int row_newindex(lua_State* L) {
    LuaRow* r = (LuaRow*)luaL_checkudata(L, 1, kRowMeta);
    luaL_checktype(L, 2, LUA_TSTRING);
    lua_getiuservalue(L, 1, 1);
    const LuaField* f = find_field(L, r->ktable, -1, 2);
    if (!f) return luaL_error(L, "%s: no field '%s'", r->ktable->table->name, lua_tostring(L, 2));
    if (!r->ktable->table->valid(r->handle)) return luaL_error(L, "%s: the row is gone (a stale handle)", r->ktable->table->name);
    script_set_field(L, *f->field, row_of(*f, r->handle), 3);
    return 0;
}

constexpr luaL_Reg k_ktable_methods[] = {
    {"spawn", ktable_spawn}, {"destroy", ktable_destroy}, {"valid", ktable_valid}, {"live", ktable_live},
    {"capacity", ktable_capacity}, {"name", ktable_name}, {"get", ktable_get}, {"set", ktable_set},
    {"each", ktable_each}, {"fields", ktable_fields}, {"row", ktable_row}, {nullptr, nullptr},
};

} // namespace

void script_push_ktable(lua_State* L, KTable* table) {
    u32 count = 0;
    for (const Column& c : table->columns) count += c.desc->fields.len;
    ScriptKTable* k = (ScriptKTable*)lua_newuserdatauv(L, sizeof(ScriptKTable) + sizeof(LuaField) * (count ? count : 1), 1);
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
            // The bare name, unless another component of the ktable has a field so named.
            u32 named = 0;
            for (const Column& c2 : table->columns)
                for (const FieldDesc& f2 : c2.desc->fields) if (strcmp(f2.name, f.name) == 0) ++named;
            if (named == 1) { lua_pushinteger(L, n); lua_setfield(L, map, f.name); }
        }
    }
    lua_setiuservalue(L, self, 1);
    if (luaL_newmetatable(L, kKTableMeta)) {
        lua_newtable(L);
        luaL_setfuncs(L, k_ktable_methods, 0);
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
