// Krome's Lua state: the helpers every script sees, a screen's script run in an environment
// of its own, its tree built from the tables it returns, its model, its handlers.
//
// Lua's errors are longjmps: the C functions here that Lua calls hold nothing with a
// destructor, and everything Krome calls into Lua goes through lua_pcall.
#include "internal.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/lua/lua.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

// The tree helpers, run once with debug.getinfo as their only argument (the debug library
// itself is not left where scripts can reach it). Each makes a node in the canonical shape
// (w, f, p, t, e, id, s, c, l) that the C++ side builds from.
const char k_prelude[] = R"lua(
local getinfo = ...
local states = { hover = true, pressed = true, focused = true, disabled = true, selected = true }
local function make(w, dir, defaults)
  return function(t)
    if type(t) ~= "table" then t = { t } end
    local info = getinfo(2, "l")
    local n = { w = w, f = {}, p = {}, c = {}, l = info and info.currentline or 0 }
    if dir then n.p.dir = dir end
    if defaults then
      for k, v in pairs(defaults) do
        if v == true then n.f[#n.f + 1] = k else n.p[k] = v end
      end
    end
    for _, v in ipairs(t) do
      if type(v) == "table" then n.c[#n.c + 1] = v
      elseif n.t == nil then n.t = tostring(v)
      else error("a node has one content string", 2) end
    end
    for k, v in pairs(t) do
      if type(k) == "string" then
        if k == "looks" and type(v) == "table" then
          -- States' looks under a key of their own, beside the flags of the same names
          -- (disabled = true with looks = { disabled = { ... } }).
          for sk, sv in pairs(v) do
            if states[sk] and type(sv) == "table" then n.s = n.s or {}; n.s[sk] = sv end
          end
        elseif k == "id" then n.id = tostring(v)
        elseif k == "event" then n.e = tostring(v)
        elseif states[k] and type(v) == "table" then n.s = n.s or {}; n.s[k] = v
        elseif v == true then n.f[#n.f + 1] = k
        elseif v ~= false then n.p[k] = v end
      end
    end
    return n
  end
end
row    = make("box", "row")
col    = make("box", "col")
stack  = make("box", "stack")
box    = make("box")
hit    = make("hit")
text   = make("text")
image  = make("image")
spacer = make("box", nil, { fill = true })
)lua";

void* lua_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    Allocator* a = (Allocator*)ud;
    if (nsize == 0) {
        if (ptr) free_in(a, ptr, osize);
        return nullptr;
    }
    void* n = alloc_in(a, nsize, 16);
    if (n && ptr) {
        memcpy(n, ptr, osize < nsize ? osize : nsize);
        free_in(a, ptr, osize);
    }
    return n;
}

KromeModule* state_of(lua_State* L, int upvalue) { return (KromeModule*)lua_touserdata(L, lua_upvalueindex(upvalue)); }

// model's __newindex: the store, then whatever reads the key. Upvalues: the store, the state,
// the screen's id.
int model_newindex(lua_State* L) {
    lua_pushvalue(L, 2);
    lua_pushvalue(L, 3);
    lua_rawset(L, lua_upvalueindex(1));
    KromeModule* s = state_of(L, 2);
    KromeScreen* sc = krome_screen_by_id(*s, (u32)lua_tointeger(L, lua_upvalueindex(3)));
    if (sc && lua_type(L, 2) == LUA_TSTRING) krome_model_changed(*s, *sc, name_intern(lua_tostring(L, 2)).hash);
    return 0;
}

// emit(event[, value]): KromeEvent from the script. Upvalues: the state, the screen's id.
int script_emit(lua_State* L) {
    const char* event = luaL_checkstring(L, 1);
    f64 value = (f64)luaL_optnumber(L, 2, 0.0);
    KromeModule* s = state_of(L, 1);
    KromeScreen* sc = krome_screen_by_id(*s, (u32)lua_tointeger(L, lua_upvalueindex(2)));
    if (sc) krome_emit(*s, KromeEvent{sc->name.hash, 0, name_intern(event).hash, value});
    return 0;
}

// A value as {key} shows it.
const char* value_text(lua_State* L, int idx, char* out, usize cap) {
    switch (lua_type(L, idx)) {
    case LUA_TNUMBER: {
        if (lua_isinteger(L, idx)) { format(out, cap, "%lld", (long long)lua_tointeger(L, idx)); return out; }
        f64 v = (f64)lua_tonumber(L, idx);
        if (v == (f64)(i64)v && v < 1e15 && v > -1e15) format(out, cap, "%lld", (long long)v);
        else format(out, cap, "%.6g", v);
        return out;
    }
    case LUA_TBOOLEAN: return lua_toboolean(L, idx) ? "true" : "false";
    case LUA_TSTRING: return lua_tostring(L, idx);
    default: return "";
    }
}

// ---- the tree from tables -------------------------------------------------------------------------

struct Build {
    KromeModule&    s;
    KromeScreen&    sc;
    KromeTree&      tree;  // the screen's, or an overlay's
    Array<KromeDep>* deps; // where texts that read the model are noted; null: filled once
    lua_State*      L;
    const char*     chunk;
    u32             errors = 0;

    void error(u32 line, const char* fmt, ...) KV_PRINTF(3, 4) {
        char msg[256];
        va_list args;
        va_start(args, fmt);
        vformat(msg, sizeof msg, fmt, args);
        va_end(args);
        KV_ERROR("krome", "%s:%u: %s", chunk, line, msg);
        ++errors;
    }
};

KromeType type_of(const char* w, bool* ok) {
    *ok = true;
    if (!strcmp(w, "box")) return KromeType::Box;
    if (!strcmp(w, "text")) return KromeType::Text;
    if (!strcmp(w, "image")) return KromeType::Image;
    if (!strcmp(w, "hit")) return KromeType::Hit;
    *ok = false;
    return KromeType::Box;
}

// The value at the top of the stack as a property's: a number, a string ("120..400" is a
// range), or a list of up to four numbers. False for anything else.
bool to_value(lua_State* L, KromeValue* out) {
    int t = lua_type(L, -1);
    if (t == LUA_TNUMBER) { *out = KromeValue::num((f32)lua_tonumber(L, -1)); return true; }
    if (t == LUA_TSTRING) {
        const char* s = lua_tostring(L, -1);
        // "120..400" is a range ("120.." no maximum), "8" a number, anything else a string.
        const char* dots = strstr(s, "..");
        if (dots && dots != s && dots - s < 32) {
            char lo_text[32];
            memcpy(lo_text, s, (usize)(dots - s));
            lo_text[dots - s] = 0;
            char* end = nullptr;
            f64 lo = strtod(lo_text, &end);
            if (*end == 0) {
                f64 hi = 1e30;
                if (dots[2]) { hi = strtod(dots + 2, &end); if (*end != 0) { *out = KromeValue::str(s); return true; } }
                *out = KromeValue::range((f32)lo, (f32)hi);
                return true;
            }
        }
        if (*s) {
            char* end = nullptr;
            f64 n = strtod(s, &end);
            if (*end == 0) { *out = KromeValue::num((f32)n); return true; }
        }
        *out = KromeValue::str(s);
        return true;
    }
    if (t == LUA_TTABLE) {
        f32 v[4];
        u32 n = 0;
        lua_Integer len = (lua_Integer)lua_rawlen(L, -1);
        if (len < 1 || len > 4) return false;
        for (lua_Integer i = 1; i <= len; ++i) {
            lua_rawgeti(L, -1, i);
            bool num = lua_type(L, -1) == LUA_TNUMBER;
            if (num) v[n++] = (f32)lua_tonumber(L, -1);
            lua_pop(L, 1);
            if (!num) return false;
        }
        *out = KromeValue::nums(v, n);
        return true;
    }
    return false;
}

// Every property in the table at the top of the stack, into a state's look.
void props(Build& b, u32 node, u32 line, KromeState state) {
    lua_State* L = b.L;
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char* name = lua_tostring(L, -2);
            KromeValue v;
            if (!to_value(L, &v)) b.error(line, "%s: not a value a property takes", name);
            else if (const char* why = b.tree.set(node, name, v, state)) b.error(line, "%s%s%s: %s", state == KromeState::Base ? "" : krome_state_name(state),
                                                                                  state == KromeState::Base ? "" : ".", name, why);
        }
        lua_pop(L, 1);
    }
}

// The names a content string reads, as {name}.
bool reads_model(const char* s) {
    const char* p = strchr(s, '{');
    return p && strchr(p + 1, '}');
}
void deps_of(Array<KromeDep>& deps, u32 node, const char* s) {
    for (const char* p = s; (p = strchr(p, '{'));) {
        const char* end = strchr(p + 1, '}');
        if (!end) break;
        if (end > p + 1) deps.push(KromeDep{node, name_intern(p + 1, (usize)(end - p - 1)).hash});
        p = end + 1;
    }
}
void fill_node(KromeModule& s, KromeScreen& sc, KromeTree& tree, u32 node);

// The node table at the top of the stack, as a child of `parent`.
void build_node(Build& b, u32 parent) {
    lua_State* L = b.L;
    KromeTree& t = b.tree;
    if (!lua_istable(L, -1)) { b.error(parent != KROME_NONE ? t.nodes[parent].line : 0, "a child that is not a node"); return; }
    lua_getfield(L, -1, "l");
    u32 line = lua_isinteger(L, -1) ? (u32)lua_tointeger(L, -1) : 0;
    lua_pop(L, 1);
    lua_getfield(L, -1, "w");
    bool ok = false;
    KromeType type = lua_type(L, -1) == LUA_TSTRING ? type_of(lua_tostring(L, -1), &ok) : KromeType::Box;
    if (!ok) b.error(line, "'%s' is not a node type (box, text, image, hit)", lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "?");
    lua_pop(L, 1);
    u32 n = t.add(type, parent);
    if (n == KROME_NONE) { b.error(line, "nesting deeper than %u", KROME_MAX_DEPTH); return; }
    // Children first: a node made in a tail call (`return col { ... }`) has no line of its
    // own, and takes its first child's, which is as near as Lua can say.
    if (lua_getfield(L, -1, "c") == LUA_TTABLE) {
        lua_Integer len = (lua_Integer)lua_rawlen(L, -1);
        for (lua_Integer i = 1; i <= len; ++i) {
            lua_rawgeti(L, -1, i);
            build_node(b, n);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    if (line == 0 || line > 0x7FFFFFFFu)
        for (u32 ch = t.nodes[n].first; ch != KROME_NONE; ch = t.nodes[ch].next)
            if (t.nodes[ch].line) { line = t.nodes[ch].line; break; }
    if (line > 0x7FFFFFFFu) line = 0;
    t.nodes[n].line = line;

    if (lua_getfield(L, -1, "p") == LUA_TTABLE) props(b, n, line, KromeState::Base);
    lua_pop(L, 1);
    if (lua_getfield(L, -1, "f") == LUA_TTABLE) {
        lua_Integer len = (lua_Integer)lua_rawlen(L, -1);
        for (lua_Integer i = 1; i <= len; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_type(L, -1) == LUA_TSTRING) {
                if (const char* why = t.flag(n, lua_tostring(L, -1))) b.error(line, "%s: %s", lua_tostring(L, -1), why);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    if (lua_getfield(L, -1, "t") == LUA_TSTRING) {
        usize len = 0;
        const char* s = lua_tolstring(L, -1, &len);
        t.set_text(n, s, (u32)len);
        if (b.deps) deps_of(*b.deps, n, s);
        else if (reads_model(s)) fill_node(b.s, b.sc, t, n);
    }
    lua_pop(L, 1);
    if (lua_getfield(L, -1, "id") == LUA_TSTRING) t.set(n, "id", KromeValue::str(lua_tostring(L, -1)));
    lua_pop(L, 1);
    if (lua_getfield(L, -1, "e") == LUA_TSTRING) t.set(n, "event", KromeValue::str(lua_tostring(L, -1)));
    lua_pop(L, 1);
    if (lua_getfield(L, -1, "s") == LUA_TTABLE) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            KromeState st;
            if (lua_type(L, -2) == LUA_TSTRING && krome_state_of(lua_tostring(L, -2), &st) && lua_istable(L, -1)) props(b, n, line, st);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

// Each node that reads the model shows its text with the values filled in.
void fill_node(KromeModule& s, KromeScreen& sc, KromeTree& tree, u32 node) {
    const char* raw = tree.raw_text(node);
    char out[1024];
    usize n = 0;
    char val[128];
    for (const char* p = raw; *p && n + 1 < sizeof out;) {
        const char* end = *p == '{' ? strchr(p + 1, '}') : nullptr;
        if (!end) { out[n++] = *p++; continue; }
        char key[96];
        usize klen = (usize)(end - p - 1);
        if (klen >= sizeof key) klen = sizeof key - 1;
        memcpy(key, p + 1, klen);
        key[klen] = 0;
        const char* v = krome_model_value_text(s, sc, key, val, sizeof val);
        for (; *v && n + 1 < sizeof out; ++v) out[n++] = *v;
        p = end + 1;
    }
    tree.set_shown(node, out, (u32)n);
}

// open_overlay(tree[, { anchor = id, side = "below", modal = false, dim = "#00000080" }]):
// the overlay's id. Upvalues: the module, the screen's id.
int script_open_overlay(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    KromeModule* s = state_of(L, 1);
    KromeScreen* sc = krome_screen_by_id(*s, (u32)lua_tointeger(L, lua_upvalueindex(2)));
    if (!sc) return 0;
    u32 id = krome_open_overlay(*s, *sc);
    KromeOverlay* o = krome_overlay(*sc, id);
    if (lua_istable(L, 2)) {
        if (lua_getfield(L, 2, "anchor") == LUA_TSTRING) o->anchor = name_intern(lua_tostring(L, -1));
        lua_pop(L, 1);
        if (lua_getfield(L, 2, "side") == LUA_TSTRING) {
            const char* side = lua_tostring(L, -1);
            o->side = !strcmp(side, "above") ? KromeSide::Above : !strcmp(side, "right") ? KromeSide::Right
                    : !strcmp(side, "left") ? KromeSide::Left : !strcmp(side, "pointer") ? KromeSide::Pointer : KromeSide::Below;
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "modal");
        o->modal = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        if (lua_getfield(L, 2, "dim") == LUA_TSTRING) krome_parse_color(lua_tostring(L, -1), &o->dim);
        lua_pop(L, 1);
    }
    lua_pushvalue(L, 1);
    krome_lua_build_overlay(*s, *sc, *o);
    lua_pop(L, 1);
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

// close_overlay([id]): that one, or the top one.
int script_close_overlay(lua_State* L) {
    KromeModule* s = state_of(L, 1);
    KromeScreen* sc = krome_screen_by_id(*s, (u32)lua_tointeger(L, lua_upvalueindex(2)));
    if (sc) krome_close_overlay(*s, *sc, (u32)luaL_optinteger(L, 1, 0));
    return 0;
}

} // namespace

bool krome_lua_open(KromeModule& s) {
    lua_State* L = lua_newstate(lua_alloc, s.general, 0);
    if (!L) return false;
    s.L = L;
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 5);
    // No files from a screen: it reads what the engine gives it.
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    if (luaL_loadbufferx(L, k_prelude, sizeof k_prelude - 1, "=krome", "t") != LUA_OK) {
        KV_ERROR("krome", "the helpers: %s", lua_tostring(L, -1));
        return false;
    }
    luaL_requiref(L, LUA_DBLIBNAME, luaopen_debug, 0);
    lua_getfield(L, -1, "getinfo");
    lua_remove(L, -2);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        KV_ERROR("krome", "the helpers: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

void krome_lua_close(KromeModule& s) {
    if (s.L) lua_close(s.L);
    s.L = nullptr;
}

void krome_lua_release(KromeModule& s, KromeScreen& sc) {
    if (!s.L) return;
    luaL_unref(s.L, LUA_REGISTRYINDEX, sc.env);
    luaL_unref(s.L, LUA_REGISTRYINDEX, sc.store);
    luaL_unref(s.L, LUA_REGISTRYINDEX, sc.on);
    sc.env = sc.store = sc.on = LUA_NOREF;
}

bool krome_lua_run(KromeModule& s, KromeScreen& sc, const char* source, usize len, const char* chunk) {
    lua_State* L = s.L;
    krome_lua_release(s, sc);
    sc.tree.clear();
    sc.deps.clear();
    char name[300];
    format(name, sizeof name, "@%s", chunk);
    if (luaL_loadbufferx(L, source, len, name, "t") != LUA_OK) {
        KV_ERROR("krome", "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        ++s.script_errors;
        return false;
    }
    // Its environment: its own globals over the shared ones, the model, emit.
    lua_newtable(L);                     // chunk env
    lua_newtable(L);                     // chunk env meta
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_newtable(L);                     // chunk env store
    lua_pushvalue(L, -1);
    sc.store = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L);                     // chunk env store proxy
    lua_newtable(L);                     // ... proxy meta
    lua_pushvalue(L, -3);
    lua_setfield(L, -2, "__index");
    lua_pushvalue(L, -3);
    lua_pushlightuserdata(L, &s);
    lua_pushinteger(L, sc.id);
    lua_pushcclosure(L, model_newindex, 3);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    lua_setfield(L, -3, "model");        // chunk env store
    lua_pop(L, 1);                       // chunk env
    lua_pushlightuserdata(L, &s);
    lua_pushinteger(L, sc.id);
    lua_pushcclosure(L, script_emit, 2);
    lua_setfield(L, -2, "emit");
    lua_pushlightuserdata(L, &s);
    lua_pushinteger(L, sc.id);
    lua_pushcclosure(L, script_open_overlay, 2);
    lua_setfield(L, -2, "open_overlay");
    lua_pushlightuserdata(L, &s);
    lua_pushinteger(L, sc.id);
    lua_pushcclosure(L, script_close_overlay, 2);
    lua_setfield(L, -2, "close_overlay");
    lua_pushvalue(L, -1);
    sc.env = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_setupvalue(L, -2, 1);            // chunk, its _ENV the env
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        KV_ERROR("krome", "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        ++s.script_errors;
        return false;
    }
    if (!lua_istable(L, -1)) {
        KV_ERROR("krome", "%s: the script returned no tree", chunk);
        lua_pop(L, 1);
        ++s.script_errors;
        return false;
    }
    // A node, or a screen table: root, model, on.
    lua_getfield(L, -1, "w");
    bool node = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (!node) {
        if (lua_getfield(L, -1, "model") == LUA_TTABLE) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, sc.store);
            lua_pushnil(L);
            while (lua_next(L, -3)) {
                lua_pushvalue(L, -2);
                lua_insert(L, -2);
                lua_rawset(L, -4);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        if (lua_getfield(L, -1, "on") == LUA_TTABLE) sc.on = luaL_ref(L, LUA_REGISTRYINDEX);
        else lua_pop(L, 1);
        lua_getfield(L, -1, "root");
        lua_remove(L, -2);
        if (!lua_istable(L, -1)) {
            KV_ERROR("krome", "%s: the screen table has no root", chunk);
            lua_pop(L, 1);
            ++s.script_errors;
            return false;
        }
    }
    Build b{s, sc, sc.tree, &sc.deps, L, chunk};
    build_node(b, KROME_NONE);
    lua_pop(L, 1);
    if (b.errors) s.script_errors += b.errors;
    for (const KromeDep& d : sc.deps) fill_node(s, sc, sc.tree, d.node);
    sc.tree.dirty = true;
    return sc.tree.root != KROME_NONE;
}

const char* krome_model_value_text(KromeModule& s, KromeScreen& sc, const char* key, char* out, usize cap) {
    lua_State* L = s.L;
    if (!L || sc.store == LUA_NOREF) return "";
    lua_rawgeti(L, LUA_REGISTRYINDEX, sc.store);
    lua_getfield(L, -1, key);
    const char* v = value_text(L, -1, out, cap);
    if (v != out) { format(out, cap, "%s", v); v = out; } // a Lua string may go when popped
    lua_pop(L, 2);
    return v;
}

void krome_model_changed(KromeModule& s, KromeScreen& sc, u64 key) {
    for (const KromeDep& d : sc.deps)
        if (d.key == key) fill_node(s, sc, sc.tree, d.node);
}

bool krome_lua_build_overlay(KromeModule& s, KromeScreen& sc, KromeOverlay& o) {
    Build b{s, sc, o.tree, nullptr, s.L, name_str(sc.name)};
    build_node(b, KROME_NONE);
    s.script_errors += b.errors;
    return o.tree.root != KROME_NONE;
}

void krome_model_set_number(KromeModule& s, KromeScreen& sc, const char* key, f64 v, bool boolean) {
    lua_State* L = s.L;
    if (!L || sc.store == LUA_NOREF) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, sc.store);
    if (boolean) lua_pushboolean(L, v != 0.0);
    else if (v == (f64)(i64)v && v < 9e15 && v > -9e15) lua_pushinteger(L, (lua_Integer)v);
    else lua_pushnumber(L, (lua_Number)v);
    lua_setfield(L, -2, key);
    lua_pop(L, 1);
    krome_model_changed(s, sc, name_intern(key).hash);
}

void krome_model_set_string(KromeModule& s, KromeScreen& sc, const char* key, const char* v) {
    lua_State* L = s.L;
    if (!L || sc.store == LUA_NOREF) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, sc.store);
    lua_pushstring(L, v);
    lua_setfield(L, -2, key);
    lua_pop(L, 1);
    krome_model_changed(s, sc, name_intern(key).hash);
}

bool krome_fire(KromeModule& s, KromeScreen& sc, KromeTree& tree, u32 node, f64 value) {
    if (node >= tree.nodes.len) return false;
    const KromeNode& n = tree.nodes[node];
    if (n.event.is_null()) return false;
    Name id = n.id, event = n.event;
    lua_State* L = s.L;
    if (L && sc.on != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, sc.on);
        lua_getfield(L, -1, name_str(event));
        lua_remove(L, -2);
        if (lua_isfunction(L, -1)) {
            if (id.is_null()) lua_pushnil(L);
            else lua_pushstring(L, name_str(id));
            lua_pushnumber(L, (lua_Number)value);
            if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
                KV_ERROR("krome", "%s", lua_tostring(L, -1));
                lua_pop(L, 1);
                ++s.script_errors;
            }
        } else {
            lua_pop(L, 1);
        }
    }
    ++s.fired;
    krome_emit(s, KromeEvent{sc.name.hash, id.hash, event.hash, value});
    return true;
}

} // namespace kv
