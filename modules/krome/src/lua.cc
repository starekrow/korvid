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
#include "korvid/core/arena.h"
#include "korvid/core/kbor.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

// The tree helpers, run once with debug.getinfo as their only argument (the debug library
// itself is not left where scripts can reach it). Each makes a node in the shape a .kimb
// holds (w, id, f, p, s, t, e, c), with `looks` for a state's look and `l` its line; the
// C++ side makes a document of it and builds from that (build.cc).
const char k_prelude[] = R"lua(
local getinfo = ...
local states = { hover = true, pressed = true, focused = true, disabled = true, selected = true }
local function words(v)
  if type(v) == "table" then return v end
  local out = {}
  for w in tostring(v):gmatch("%S+") do out[#out + 1] = w end
  return out
end
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
            if states[sk] and type(sv) == "table" then n.looks = n.looks or {}; n.looks[sk] = sv end
          end
        elseif k == "style" or k == "styles" then n.s = words(v)
        elseif k == "id" then n.id = tostring(v)
        elseif k == "event" then n.e = tostring(v)
        elseif states[k] and type(v) == "table" then n.looks = n.looks or {}; n.looks[k] = v
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
-- A screen of a .kimb as a piece of this tree, built in that file's scope.
function kim(file, screen)
  local info = getinfo(2, "l")
  return { w = "@kim", file = file, screen = screen, l = info and info.currentline or 0 }
end
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
// A script's tables made a document, so the builder that builds a .kimb builds them too:
// a table with an array part is an array, any other an object of its string keys; a
// node's `l` is its line.

bool empty_table(lua_State* L, int idx) {
    idx = lua_absindex(L, idx);
    lua_pushnil(L);
    if (!lua_next(L, idx)) return true;
    lua_pop(L, 2);
    return false;
}

JsonRef to_json(lua_State* L, int idx, JsonBuilder& b, u32 depth) {
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx)) return b.integer((i64)lua_tointeger(L, idx));
        return b.number((f64)lua_tonumber(L, idx));
    case LUA_TBOOLEAN: return b.boolean(lua_toboolean(L, idx) != 0);
    case LUA_TSTRING: {
        usize len = 0;
        const char* str = lua_tolstring(L, idx, &len);
        return b.string(str, (u32)len);
    }
    case LUA_TTABLE: break;
    default: return b.null();
    }
    if (depth > KROME_MAX_DEPTH * 2) return b.null();
    lua_Integer len = (lua_Integer)lua_rawlen(L, idx);
    if (len > 0) {
        JsonRef a = b.array();
        for (lua_Integer i = 1; i <= len; ++i) {
            lua_rawgeti(L, idx, i);
            b.push(a, to_json(L, -1, b, depth + 1));
            lua_pop(L, 1);
        }
        return a;
    }
    JsonRef o = b.object();
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char* key = lua_tostring(L, -2);
            if (!strcmp(key, "l") && lua_isinteger(L, -1)) {
                lua_Integer l = lua_tointeger(L, -1);
                b.line(o, l > 0 && l < 0x7FFFFFFF ? (u32)l : 0);
            } else if (lua_type(L, -1) == LUA_TTABLE && empty_table(L, -1) && (!strcmp(key, "f") || !strcmp(key, "c") || !strcmp(key, "p"))) {
                // An empty list or map the helpers always make: nothing to say.
            } else {
                b.set(o, key, to_json(L, -1, b, depth + 1));
            }
        }
        lua_pop(L, 1);
    }
    return o;
}

// A node made in a tail call (`return col { ... }`) has no line of its own: it takes its
// first child's, which is as near as Lua can say.
void lines_from_children(JsonDoc& doc, JsonValue& n) {
    const JsonValue* c = json_get(doc, n, "c");
    if (c) for (const JsonValue* k = json_first(doc, *c); k; k = json_next(doc, *k)) lines_from_children(doc, const_cast<JsonValue&>(*k));
    if (n.line == 0 && c)
        for (const JsonValue* k = json_first(doc, *c); k; k = json_next(doc, *k))
            if (k->line) { n.line = k->line; break; }
}

// The node table at `idx` as a document of one screen, "root", in `arena`; with `uses`
// (a Lua table of names at uses_idx, or 0) as the file's.
bool table_file(lua_State* L, int idx, int uses_idx, Arena& arena, KimbFile& f, const char* chunk, KromeModule& s) {
    idx = lua_absindex(L, idx);
    if (uses_idx) uses_idx = lua_absindex(L, uses_idx);
    if (arena.base) arena_destroy(arena);
    if (!arena_create(arena, 64u << 20, "krome.lua_doc", MemTag{})) return false;
    JsonBuilder b(&arena.a);
    JsonRef root = b.object();
    b.set(root, "kim", b.integer(1));
    b.set(root, "file", b.string(chunk));
    if (uses_idx && lua_istable(L, uses_idx)) {
        JsonRef u = b.array();
        lua_Integer n = (lua_Integer)lua_rawlen(L, uses_idx);
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, uses_idx, i);
            if (lua_type(L, -1) == LUA_TSTRING) b.push(u, b.string(lua_tostring(L, -1)));
            lua_pop(L, 1);
        }
        b.set(root, "uses", u);
    }
    JsonRef screens = b.object();
    b.set(screens, "root", to_json(L, idx, b, 0));
    b.set(root, "screens", screens);
    JsonDoc doc;
    bool ok = b.finish(root, doc);
    b.free();
    if (!ok) return false;
    KimErrors quiet;
    if (!kimb_from_doc(doc, f, quiet, chunk)) return false;
    if (const JsonValue* r = json_get(f.doc, *f.screens, "root")) lines_from_children(f.doc, const_cast<JsonValue&>(*r));
    (void)s;
    return true;
}

} // namespace

// Each node that reads the model shows its text with the values filled in.
void krome_fill_node(KromeModule& s, KromeScreen& sc, KromeTree& tree, u32 node) {
    const char* raw = tree.raw_text(node);
    char out[1024];
    usize n = 0;
    char val[128];
    for (const char* p = raw; *p && n + 1 < sizeof out;) {
        if (p[0] == '{' && p[1] == '{') { out[n++] = '{'; p += 2; continue; }
        const char* end = *p == '{' ? strchr(p + 1, '}') : nullptr;
        if (!end) { out[n++] = *p++; continue; }
        char key[96];
        usize klen = (usize)(end - p - 1);
        if (klen >= sizeof key) klen = sizeof key - 1;
        memcpy(key, p + 1, klen);
        key[klen] = 0;
        if (key[0] != '*') { // {*tag}: a span's look (slice 3), nothing to show
            const char* v = krome_model_value_text(s, sc, key, val, sizeof val);
            for (; *v && n + 1 < sizeof out; ++v) out[n++] = *v;
        }
        p = end + 1;
    }
    tree.set_shown(node, out, (u32)n);
}

namespace {

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
    if (!table_file(L, 1, 0, o->arena, o->file, name_str(sc->name), *s) || !krome_build_overlay(*s, *sc, *o))
        krome_error(*s, name_str(sc->name), 0, nullptr, 0, "open_overlay: the tree would not build");
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

// theme(name): every screen in that theme, from the next frame. Upvalues: the module.
int script_theme(lua_State* L) {
    KromeModule* s = state_of(L, 1);
    const char* name = luaL_checkstring(L, 1);
    s->theme = name_intern(name);
    for (KromeScreen* sc : s->screens) sc->rebuild = true;
    return 0;
}

// changed(key): what reads the key follows, as if it were written (a list changed in
// place). Upvalues: the module, the screen's id.
int script_changed(lua_State* L) {
    KromeModule* s = state_of(L, 1);
    KromeScreen* sc = krome_screen_by_id(*s, (u32)lua_tointeger(L, lua_upvalueindex(2)));
    if (sc) krome_model_changed(*s, *sc, name_intern(luaL_checkstring(L, 1)).hash);
    return 0;
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

namespace {

// A fresh environment for the screen: its own globals over the shared ones, the model and
// its store, emit, the overlays, theme. Left in sc.env and sc.store.
void make_env(KromeModule& s, KromeScreen& sc) {
    lua_State* L = s.L;
    krome_lua_release(s, sc);
    lua_newtable(L);                     // env
    lua_newtable(L);                     // env meta
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_newtable(L);                     // env store
    lua_pushvalue(L, -1);
    sc.store = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L);                     // env store proxy
    lua_newtable(L);                     // ... proxy meta
    lua_pushvalue(L, -3);
    lua_setfield(L, -2, "__index");
    lua_pushvalue(L, -3);
    lua_pushlightuserdata(L, &s);
    lua_pushinteger(L, sc.id);
    lua_pushcclosure(L, model_newindex, 3);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    lua_setfield(L, -3, "model");        // env store
    lua_pop(L, 1);                       // env
    struct Fn { const char* name; lua_CFunction fn; int ups; };
    const Fn fns[] = {{"emit", script_emit, 2}, {"open_overlay", script_open_overlay, 2}, {"close_overlay", script_close_overlay, 2}, {"theme", script_theme, 1}, {"changed", script_changed, 2}};
    for (const Fn& f : fns) {
        lua_pushlightuserdata(L, &s);
        if (f.ups == 2) lua_pushinteger(L, sc.id);
        lua_pushcclosure(L, f.fn, f.ups);
        lua_setfield(L, -2, f.name);
    }
    sc.env = luaL_ref(L, LUA_REGISTRYINDEX);
}

// Runs a chunk in the screen's environment, leaving what it returned on the stack. False
// (logged) when it would not load or raised.
bool run_chunk(KromeModule& s, KromeScreen& sc, const char* source, usize len, const char* chunk) {
    lua_State* L = s.L;
    char name[300];
    format(name, sizeof name, "@%s", chunk);
    if (luaL_loadbufferx(L, source, len, name, "t") != LUA_OK) {
        KV_ERROR("krome", "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        ++s.script_errors;
        return false;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, sc.env);
    lua_setupvalue(L, -2, 1);            // the chunk's _ENV
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        KV_ERROR("krome", "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        ++s.script_errors;
        return false;
    }
    return true;
}

// The table at the top of the stack's model into the store, a later key over an earlier.
void merge_model(lua_State* L, KromeScreen& sc) {
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
}

} // namespace

bool krome_lua_run(KromeModule& s, KromeScreen& sc, const char* source, usize len, const char* chunk) {
    lua_State* L = s.L;
    make_env(s, sc);
    sc.uses.clear();
    if (!run_chunk(s, sc, source, len, chunk)) return false;
    if (!lua_istable(L, -1)) {
        krome_error(s, chunk, 0, nullptr, 0, "the script returned no tree");
        lua_pop(L, 1);
        return false;
    }
    // A node, or a screen table: root, model, on, uses.
    lua_getfield(L, -1, "w");
    bool node = !lua_isnil(L, -1);
    lua_pop(L, 1);
    int uses = 0;
    if (!node) {
        merge_model(L, sc);
        if (lua_getfield(L, -1, "on") == LUA_TTABLE) sc.on = luaL_ref(L, LUA_REGISTRYINDEX);
        else lua_pop(L, 1);
        lua_getfield(L, -1, "uses");     // screen uses
        lua_getfield(L, -2, "root");     // screen uses root
        if (!lua_istable(L, -1)) {
            krome_error(s, chunk, 0, nullptr, 0, "the screen table has no root");
            lua_pop(L, 3);
            return false;
        }
        lua_insert(L, -3);               // root screen uses
        lua_remove(L, -2);               // root uses
        lua_insert(L, -2);               // uses root
        uses = -2;
    }
    bool ok = table_file(L, -1, uses, sc.arena, sc.lua_file, chunk, s);
    lua_pop(L, node ? 1 : 2);
    if (!ok) { krome_error(s, chunk, 0, nullptr, 0, "its tree would not make a document"); return false; }
    sc.has_lua_file = true;
    return true;
}

bool krome_lua_scripts(KromeModule& s, KromeScreen& sc, KromeDoc* const* scripts, u32 count) {
    lua_State* L = s.L;
    make_env(s, sc);
    lua_newtable(L);                     // on
    lua_newtable(L);                     // on, where each handler came from
    bool ok = true;
    for (u32 i = 0; i < count; ++i) {
        const char* chunk = name_str(scripts[i]->path);
        if (!run_chunk(s, sc, scripts[i]->source, scripts[i]->source_len, chunk)) { ok = false; continue; }
        if (lua_isnil(L, -1)) { lua_pop(L, 1); continue; } // a script that only set things up
        if (!lua_istable(L, -1)) {
            krome_error(s, chunk, 0, nullptr, 0, "a screen's script returns { model = ..., on = ... }");
            lua_pop(L, 1);
            ok = false;
            continue;
        }
        merge_model(L, sc);
        if (lua_getfield(L, -1, "on") == LUA_TTABLE) {
            lua_pushnil(L);
            while (lua_next(L, -2)) {    // on from script key fn
                if (lua_type(L, -2) == LUA_TSTRING) {
                    const char* event = lua_tostring(L, -2);
                    lua_getfield(L, -5, event);
                    if (lua_type(L, -1) == LUA_TSTRING) {
                        krome_error(s, chunk, 0, nullptr, 0, "the event '%s' is handled here and in %s", event, lua_tostring(L, -1));
                        ok = false;
                        lua_pop(L, 1);
                    } else {
                        lua_pop(L, 1);       // on from script son key fn
                        lua_pushvalue(L, -2);
                        lua_pushvalue(L, -2);
                        lua_rawset(L, -8);   // on[event] = fn
                        lua_pushstring(L, chunk);
                        lua_setfield(L, -6, event); // from[event] = the script
                    }
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 2);                   // on from
    }
    lua_pop(L, 1);                       // on
    sc.on = luaL_ref(L, LUA_REGISTRYINDEX);
    return ok;
}

namespace {

// The model's value at a dotted path pushed (nil when any step is missing): a key, then
// for each further part an element (a number, from 1) or a field.
void push_path(lua_State* L, int store, const char* path) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, store);
    const char* p = path;
    while (*p) {
        const char* dot = strchr(p, '.');
        usize n = dot ? (usize)(dot - p) : strlen(p);
        if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_pushnil(L); return; }
        bool digits = n > 0;
        for (usize i = 0; i < n; ++i) if (p[i] < '0' || p[i] > '9') digits = false;
        if (digits) lua_rawgeti(L, -1, (lua_Integer)strtoll(p, nullptr, 10));
        else { lua_pushlstring(L, p, n); lua_gettable(L, -2); }
        lua_remove(L, -2);
        p += n;
        if (*p == '.') ++p;
    }
}

} // namespace

bool krome_model_truthy(KromeModule& s, KromeScreen& sc, const char* path) {
    lua_State* L = s.L;
    if (!L || sc.store == LUA_NOREF) return false;
    push_path(L, sc.store, path);
    bool t;
    switch (lua_type(L, -1)) {
    case LUA_TNIL: t = false; break;
    case LUA_TBOOLEAN: t = lua_toboolean(L, -1) != 0; break;
    case LUA_TNUMBER: t = lua_tonumber(L, -1) != 0; break;
    case LUA_TSTRING: t = lua_rawlen(L, -1) != 0; break;
    case LUA_TTABLE: t = lua_rawlen(L, -1) != 0; break;
    default: t = true;
    }
    lua_pop(L, 1);
    return t;
}

u32 krome_model_length(KromeModule& s, KromeScreen& sc, const char* path) {
    lua_State* L = s.L;
    if (!L || sc.store == LUA_NOREF) return 0;
    push_path(L, sc.store, path);
    u32 n = lua_istable(L, -1) ? (u32)lua_rawlen(L, -1) : 0;
    lua_pop(L, 1);
    return n;
}

const char* krome_model_path_text(KromeModule& s, KromeScreen& sc, const char* path, char* out, usize cap) {
    lua_State* L = s.L;
    if (!L || sc.store == LUA_NOREF) return "";
    push_path(L, sc.store, path);
    const char* v = value_text(L, -1, out, cap);
    if (v != out) { format(out, cap, "%s", v); v = out; }
    lua_pop(L, 1);
    return v;
}

void krome_model_flip(KromeModule& s, KromeScreen& sc, const char* path) {
    lua_State* L = s.L;
    if (!L || sc.store == LUA_NOREF) return;
    bool now = krome_model_truthy(s, sc, path);
    // The table that holds the last part, then the part.
    const char* last = strrchr(path, '.');
    if (last) {
        char parent[300];
        format(parent, sizeof parent, "%.*s", (int)(last - path), path);
        push_path(L, sc.store, parent);
        ++last;
    } else {
        lua_rawgeti(L, LUA_REGISTRYINDEX, sc.store);
        last = path;
    }
    if (lua_istable(L, -1)) {
        lua_pushboolean(L, !now);
        bool digits = *last != 0;
        for (const char* q = last; *q; ++q) if (*q < '0' || *q > '9') digits = false;
        if (digits) lua_rawseti(L, -2, (lua_Integer)strtoll(last, nullptr, 10));
        else lua_setfield(L, -2, last);
    }
    lua_pop(L, 1);
    usize head = strcspn(path, ".");
    krome_model_changed(s, sc, name_intern(path, head).hash);
}

namespace {

void push_json(lua_State* L, const JsonDoc& doc, const JsonValue& v, u32 depth) {
    switch (v.type) {
    case JsonType::Bool: lua_pushboolean(L, json_bool(&v, false)); return;
    case JsonType::Number:
        if (v.integer()) lua_pushinteger(L, (lua_Integer)json_i64(&v, 0));
        else lua_pushnumber(L, (lua_Number)v.number());
        return;
    case JsonType::String: lua_pushlstring(L, v.str(), v.len()); return;
    case JsonType::Bytes: lua_pushlstring(L, (const char*)v.bytes(), v.len()); return;
    case JsonType::Array:
    case JsonType::Object: {
        if (depth > 64) { lua_pushnil(L); return; }
        lua_createtable(L, v.is_array() ? (int)v.count() : 0, v.is_object() ? (int)v.count() : 0);
        lua_Integer i = 1;
        for (const JsonValue* k = json_first(doc, v); k; k = json_next(doc, *k)) {
            push_json(L, doc, *k, depth + 1);
            if (v.is_array()) lua_rawseti(L, -2, i++);
            else lua_setfield(L, -2, k->key);
        }
        return;
    }
    default: lua_pushnil(L); return;
    }
}

} // namespace

bool krome_model_set_doc(KromeModule& s, KromeScreen& sc, const char* key, const void* data, usize size) {
    lua_State* L = s.L;
    if (!L || sc.store == LUA_NOREF) return false;
    Arena arena;
    if (!arena_create(arena, size * 32 + (64u << 10), "krome.set_doc", MemTag{})) return false;
    JsonDoc doc;
    bool ok = kbor_parse(data, size, &arena.a, doc);
    if (ok) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, sc.store);
        push_json(L, doc, doc.root(), 0);
        lua_setfield(L, -2, key);
        lua_pop(L, 1);
    } else {
        krome_error(s, name_str(sc.name), 0, nullptr, 0, "krome_set_doc %s: %s at byte %u", key, doc.error, doc.error_at);
    }
    arena_destroy(arena);
    if (ok) krome_model_changed(s, sc, name_intern(key).hash);
    return ok;
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
    for (u32 i = 0; i < sc.deps.len; ++i) {
        const KromeDep d = sc.deps[i];
        if (d.key != key) continue;
        if (!d.binding) { krome_fill_node(s, sc, sc.tree, d.node); continue; }
        KromeNode& n = sc.tree.nodes[d.node];
        u16 was = n.flags;
        if (krome_model_truthy(s, sc, name_str(n.binding))) n.flags |= KROME_SELECTED;
        else n.flags &= (u16)~KROME_SELECTED;
        if (n.flags != was) sc.repaint = true;
    }
    krome_rebuild_regions(s, sc, key);
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
    if (n.event.is_null() && n.binding.is_null()) return false;
    Name id = n.id, event = n.event, bound = n.binding;
    // A binding first: the value flipped, so the handler sees it as the press left it.
    if (!bound.is_null()) krome_model_flip(s, sc, name_str(bound));
    if (event.is_null()) { ++s.fired; return true; }
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
