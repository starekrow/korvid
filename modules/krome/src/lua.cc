// Krome's Lua state: the helpers every script sees, a screen's script run in an environment
// of its own, its tree built from the tables it returns, its model, its handlers.
//
// Lua's errors are longjmps: the C functions here that Lua calls hold nothing with a
// destructor, and everything Krome calls into Lua goes through lua_pcall.
#include "internal.h"
#include "korvid/core/hash.h"
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
        elseif k == "bind" then n.b = tostring(v)
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
edit   = make("edit")
-- A screen of a .kimb as a piece of this tree, built in that file's scope.
function kim(file, screen)
  local info = getinfo(2, "l")
  return { w = "@kim", file = file, screen = screen, l = info and info.currentline or 0 }
end
)lua";

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
            // A dotted key is a path into the model ("sel.type": a field of a table).
            const char* v = strchr(key, '.') ? krome_model_path_text(s, sc, key, val, sizeof val) : krome_model_value_text(s, sc, key, val, sizeof val);
            for (; *v && n + 1 < sizeof out; ++v) out[n++] = *v;
        }
        p = end + 1;
    }
    tree.set_shown(node, out, (u32)n);
}

namespace {

// open_overlay(tree[, { anchor = id, side = "below", modal = false, dim = "#00000080" }]):
// the overlay's id. Upvalues: the module, the screen's id.
// The tree at `tree` opened over the screen with the options at `opts` (0: none); `anchor`
// what it opens at unless the options say. Pushes its id.
int open_overlay_at(lua_State* L, KromeModule* s, KromeScreen* sc, int tree, int opts, Name anchor) {
    u32 id = krome_open_overlay(*s, *sc);
    KromeOverlay* o = krome_overlay(*sc, id);
    o->anchor = anchor;
    if (opts && lua_istable(L, opts)) {
        if (lua_getfield(L, opts, "anchor") == LUA_TSTRING) o->anchor = name_intern(lua_tostring(L, -1));
        lua_pop(L, 1);
        if (lua_getfield(L, opts, "side") == LUA_TSTRING) {
            const char* side = lua_tostring(L, -1);
            o->side = !strcmp(side, "above") ? KromeSide::Above : !strcmp(side, "right") ? KromeSide::Right
                    : !strcmp(side, "left") ? KromeSide::Left : !strcmp(side, "pointer") ? KromeSide::Pointer : KromeSide::Below;
        }
        lua_pop(L, 1);
        lua_getfield(L, opts, "modal");
        o->modal = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        if (lua_getfield(L, opts, "dim") == LUA_TSTRING) krome_parse_color(lua_tostring(L, -1), &o->dim);
        lua_pop(L, 1);
    }
    // A modal holds the keyboard: what had it under the modal loses it.
    if (o->modal && s->focus.node != KROME_NONE) krome_edit_blur(*s);
    if (!table_file(L, tree, 0, o->arena, o->file, name_str(sc->name), *s) || !krome_build_overlay(*s, *sc, *o))
        krome_error(*s, name_str(sc->name), 0, nullptr, 0, "open_overlay: the tree would not build");
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

int script_open_overlay(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    KromeModule* s = state_of(L, 1);
    KromeScreen* sc = krome_screen_by_id(*s, (u32)lua_tointeger(L, lua_upvalueindex(2)));
    if (!sc) return 0;
    return open_overlay_at(L, s, sc, 1, 2, Name{});
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
    lua_State* L = kv_lua_newstate(s.general);
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
// its store, emit, the overlays, theme. Left in sc.env and sc.store. `keep`: a reload's, over
// the store the screen has (its env and handlers are the caller's to keep or let go).
void make_env(KromeModule& s, KromeScreen& sc, bool keep = false) {
    lua_State* L = s.L;
    if (!keep) krome_lua_release(s, sc);
    else sc.env = sc.on = LUA_NOREF;
    lua_newtable(L);                     // env
    lua_newtable(L);                     // env meta
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    if (keep && sc.store != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, sc.store); // env store
    } else {
        lua_newtable(L);                 // env store
        lua_pushvalue(L, -1);
        sc.store = luaL_ref(L, LUA_REGISTRYINDEX);
    }
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
// `only_new`: a reload's, which adds the keys the store does not have and keeps the rest.
void merge_model(lua_State* L, KromeScreen& sc, bool only_new = false) {
    if (lua_getfield(L, -1, "model") == LUA_TTABLE) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, sc.store);
        lua_pushnil(L);
        while (lua_next(L, -3)) {        // model store key value
            if (only_new) {
                lua_pushvalue(L, -2);
                bool has = lua_rawget(L, -4) != LUA_TNIL;
                lua_pop(L, 1);
                if (has) { lua_pop(L, 1); continue; }
            }
            lua_pushvalue(L, -2);
            lua_insert(L, -2);
            lua_rawset(L, -4);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

// A reload's run that failed: what it made let go, what the screen had back.
void restore_env(KromeModule& s, KromeScreen& sc, int env, int on) {
    luaL_unref(s.L, LUA_REGISTRYINDEX, sc.env);
    luaL_unref(s.L, LUA_REGISTRYINDEX, sc.on);
    sc.env = env;
    sc.on = on;
}
void drop_env(KromeModule& s, int env, int on) {
    luaL_unref(s.L, LUA_REGISTRYINDEX, env);
    luaL_unref(s.L, LUA_REGISTRYINDEX, on);
}

} // namespace

bool krome_lua_run(KromeModule& s, KromeScreen& sc, const char* source, usize len, const char* chunk, bool again) {
    lua_State* L = s.L;
    int old_env = sc.env, old_on = sc.on;
    make_env(s, sc, again);
    if (!again) sc.uses.clear();
    auto fail = [&] { if (again) restore_env(s, sc, old_env, old_on); return false; };
    if (!run_chunk(s, sc, source, len, chunk)) return fail();
    if (!lua_istable(L, -1)) {
        krome_error(s, chunk, 0, nullptr, 0, "the script returned no tree");
        lua_pop(L, 1);
        return fail();
    }
    // A node, or a screen table: root, model, on, uses.
    lua_getfield(L, -1, "w");
    bool node = !lua_isnil(L, -1);
    lua_pop(L, 1);
    int uses = 0;
    if (!node) {
        merge_model(L, sc, again);
        if (lua_getfield(L, -1, "on") == LUA_TTABLE) sc.on = luaL_ref(L, LUA_REGISTRYINDEX);
        else lua_pop(L, 1);
        lua_getfield(L, -1, "uses");     // screen uses
        lua_getfield(L, -2, "root");     // screen uses root
        if (!lua_istable(L, -1)) {
            krome_error(s, chunk, 0, nullptr, 0, "the screen table has no root");
            lua_pop(L, 3);
            return fail();
        }
        lua_insert(L, -3);               // root screen uses
        lua_remove(L, -2);               // root uses
        lua_insert(L, -2);               // uses root
        uses = -2;
    }
    // A reload's tree into a document of its own: the tree built from the old one points into
    // it until the screen is built again, so the old one is retired, not destroyed.
    Arena fresh{};
    KimbFile fresh_file;
    bool ok = again ? table_file(L, -1, uses, fresh, fresh_file, chunk, s) : table_file(L, -1, uses, sc.arena, sc.lua_file, chunk, s);
    lua_pop(L, node ? 1 : 2);
    if (!ok) {
        if (fresh.base) arena_destroy(fresh);
        krome_error(s, chunk, 0, nullptr, 0, "its tree would not make a document");
        return fail();
    }
    if (again) {
        drop_env(s, old_env, old_on);
        if (sc.arena.base) {
            KromeDocVersion* v = alloc_items<KromeDocVersion>(s.general, 1);
            *v = KromeDocVersion{};
            v->arena = sc.arena;
            v->file = sc.lua_file;
            v->retired_at = ++s.epoch;
            v->owner = sc.id;
            s.retired.alloc = s.general;
            s.retired.push(v);
        }
        sc.arena = fresh;
        sc.lua_file = fresh_file;
    }
    sc.has_lua_file = true;
    return true;
}

bool krome_lua_scripts(KromeModule& s, KromeScreen& sc, KromeDoc* const* scripts, u32 count, bool again) {
    lua_State* L = s.L;
    int old_env = sc.env, old_on = sc.on;
    make_env(s, sc, again);
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
        merge_model(L, sc, again);
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
    // A reload whose script failed keeps the handlers the screen had: what the others gave
    // would be half of them.
    if (again && !ok) { restore_env(s, sc, old_env, old_on); return false; }
    if (again) drop_env(s, old_env, old_on);
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

bool krome_bound_selected(KromeModule& s, KromeScreen& sc, const KromeTree& tree, u32 node) {
    const KromeNode& n = tree.nodes[node];
    if (n.binding.is_null()) return false;
    if (n.bind_value == KROME_NONE) return krome_model_truthy(s, sc, name_str(n.binding));
    char v[256];
    krome_model_path_text(s, sc, name_str(n.binding), v, sizeof v);
    return !strcmp(v, tree.strings.data + n.bind_value);
}

bool krome_model_value(KromeModule& s, KromeScreen& sc, const char* path, KromeValue* out, char* buf, usize cap) {
    lua_State* L = s.L;
    if (!L || sc.store == LUA_NOREF) return false;
    push_path(L, sc.store, path);
    bool ok = true;
    switch (lua_type(L, -1)) {
    case LUA_TNUMBER: *out = KromeValue::num((f32)lua_tonumber(L, -1)); break;
    case LUA_TBOOLEAN: *out = KromeValue::num(lua_toboolean(L, -1) ? 1.0f : 0.0f); break;
    case LUA_TSTRING: format(buf, cap, "%s", lua_tostring(L, -1)); krome_string_value(buf, out); break;
    case LUA_TTABLE: {
        f32 n[4];
        u32 c = 0;
        for (; c < 4; ++c) {
            lua_rawgeti(L, -1, (lua_Integer)c + 1);
            bool num = lua_type(L, -1) == LUA_TNUMBER;
            if (num) n[c] = (f32)lua_tonumber(L, -1);
            lua_pop(L, 1);
            if (!num) break;
        }
        ok = c > 0;
        if (ok) *out = KromeValue::nums(n, c);
        break;
    }
    default: ok = false;
    }
    lua_pop(L, 1);
    return ok;
}

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

namespace {
u64 value_hash(lua_State* L, int i, u32 depth) {
    i = lua_absindex(L, i);
    switch (lua_type(L, i)) {
    case LUA_TNIL:     return 0x6e696cull;
    case LUA_TBOOLEAN: return lua_toboolean(L, i) ? 0x74727565ull : 0x66616c73ull;
    case LUA_TNUMBER: {
        if (lua_isinteger(L, i)) return hash_mix((u64)lua_tointeger(L, i) ^ 0x696e74ull);
        f64 v = lua_tonumber(L, i);
        u64 bits;
        memcpy(&bits, &v, sizeof bits);
        return hash_mix(bits ^ 0x666c74ull);
    }
    case LUA_TSTRING: {
        usize n = 0;
        const char* str = lua_tolstring(L, i, &n);
        return hash_bytes(str, n);
    }
    case LUA_TTABLE: {
        if (depth > 6) return 0x646565ull;
        // A sum of the pairs' hashes: the same whatever order next gives them in.
        u64 h = 0x746162ull;
        lua_pushnil(L);
        while (lua_next(L, i)) {
            u64 k = value_hash(L, -2, depth + 1), v = value_hash(L, -1, depth + 1);
            h += hash_mix(k * 0x9E3779B97F4A7C15ull ^ v);
            lua_pop(L, 1);
        }
        return hash_mix(h);
    }
    default: return hash_mix((u64)(usize)lua_topointer(L, i));
    }
}
} // namespace

u64 krome_model_hash(KromeModule& s, KromeScreen& sc, const char* path) {
    lua_State* L = s.L;
    if (!L || sc.store == LUA_NOREF) return 0;
    push_path(L, sc.store, path);
    u64 h = lua_isnil(L, -1) ? 0 : value_hash(L, -1, 0);
    lua_pop(L, 1);
    return h;
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

void krome_model_into_text(KromeModule& s, KromeScreen& sc, const char* path, KromeTree& tree, u32 node) {
    lua_State* L = s.L;
    if (!L || sc.store == LUA_NOREF) { tree.set_text(node, ""); return; }
    push_path(L, sc.store, path);
    if (lua_type(L, -1) == LUA_TSTRING) {
        usize n = 0;
        const char* str = lua_tolstring(L, -1, &n);
        tree.set_text(node, str, (u32)n); // copied while the model still holds it
    } else {
        char val[128];
        const char* v = value_text(L, -1, val, sizeof val);
        tree.set_text(node, v);
    }
    lua_pop(L, 1);
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
        if (d.kind == KromeDep::Text) { krome_fill_node(s, sc, sc.tree, d.node); continue; }
        if (d.kind == KromeDep::Behave) {
            krome_behave(s, KromeTarget{sc.id, 0, d.node}, KromeCall::Update, KromeCallArgs{});
            continue;
        }
        if (d.kind == KromeDep::Prop) {
            if (const char* bad = krome_apply_bound(s, sc, sc.tree, d))
                krome_error(s, name_str(sc.name), 0, nullptr, 0, "%s=@%s: %s", name_str(d.prop), name_str(d.path), bad);
            sc.repaint = true;
            continue;
        }
        KromeNode& n = sc.tree.nodes[d.node];
        if (n.type == KromeType::Edit) {
            // An edit shows the model's text, unless it is the one being typed in.
            if (!krome_is_focused(s, sc.id, 0, d.node)) {
                krome_model_into_text(s, sc, name_str(n.binding), sc.tree, d.node);
                sc.tree.dirty = true;
                sc.repaint = true;
            }
            continue;
        }
        u32 was = n.flags;
        if (krome_bound_selected(s, sc, sc.tree, d.node)) n.flags |= KROME_SELECTED;
        else n.flags &= ~KROME_SELECTED;
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

void krome_fire_view(KromeModule& s, KromeScreen& sc, Name event, u32 first, u32 count, u32 total) {
    lua_State* L = s.L;
    if (L && sc.on != LUA_NOREF && !event.is_null()) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, sc.on);
        lua_getfield(L, -1, name_str(event));
        lua_remove(L, -2);
        if (lua_isfunction(L, -1)) {
            lua_pushinteger(L, first);
            lua_pushinteger(L, count);
            lua_pushinteger(L, total);
            if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
                KV_ERROR("krome", "%s", lua_tostring(L, -1));
                lua_pop(L, 1);
                ++s.script_errors;
            }
        } else {
            lua_pop(L, 1);
        }
    }
    krome_emit_view(s, KromeView{sc.name.hash, event.hash, first, count, total, 0});
}

void krome_fire_named(KromeModule& s, KromeScreen& sc, KromeTree& tree, u32 node, Name event, f64 value) {
    if (node >= tree.nodes.len || event.is_null()) return;
    Name id = tree.nodes[node].id;
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
}

void krome_fire_at(KromeModule& s, KromeScreen& sc, KromeTree& tree, u32 node, Name event, f32 x, f32 y) {
    if (node >= tree.nodes.len || event.is_null()) return;
    Name id = tree.nodes[node].id;
    lua_State* L = s.L;
    if (L && sc.on != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, sc.on);
        lua_getfield(L, -1, name_str(event));
        lua_remove(L, -2);
        if (lua_isfunction(L, -1)) {
            if (id.is_null()) lua_pushnil(L);
            else lua_pushstring(L, name_str(id));
            lua_pushnumber(L, (lua_Number)x);
            lua_pushnumber(L, (lua_Number)y);
            if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
                KV_ERROR("krome", "%s", lua_tostring(L, -1));
                lua_pop(L, 1);
                ++s.script_errors;
            }
        } else {
            lua_pop(L, 1);
        }
    }
    ++s.fired;
    KromeEvent ev{sc.name.hash, id.hash, event.hash, 0.0};
    ev.x = x;
    ev.y = y;
    krome_emit(s, ev);
}

void krome_model_write_string(KromeModule& s, KromeScreen& sc, const char* path, const char* value) {
    lua_State* L = s.L;
    if (!L || sc.store == LUA_NOREF) return;
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
        lua_pushstring(L, value);
        bool digits = *last != 0;
        for (const char* q = last; *q; ++q) if (*q < '0' || *q > '9') digits = false;
        if (digits) lua_rawseti(L, -2, (lua_Integer)strtoll(last, nullptr, 10));
        else lua_setfield(L, -2, last);
    }
    lua_pop(L, 1);
    usize head = strcspn(path, ".");
    krome_model_changed(s, sc, name_intern(path, head).hash);
}

bool krome_fire(KromeModule& s, KromeScreen& sc, KromeTree& tree, u32 node, f64 value) {
    if (node >= tree.nodes.len) return false;
    // In what a behaviour opened (a dropdown's list): its pick first.
    if (krome_pick(s, sc, tree, node)) { ++s.fired; return true; }
    const KromeNode& n = tree.nodes[node];
    if (n.event.is_null() && n.binding.is_null()) return false;
    Name id = n.id, event = n.event, bound = n.binding;
    // A binding first: the value flipped (or for `@key=value`, the value written), so the
    // handler sees it as the press left it.
    if (!bound.is_null()) {
        if (n.bind_value != KROME_NONE) {
            char v[256];
            format(v, sizeof v, "%s", tree.strings.data + n.bind_value); // the write may rebuild the tree
            krome_model_write_string(s, sc, name_str(bound), v);
        } else {
            krome_model_flip(s, sc, name_str(bound));
        }
    }
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

// ---- behaviours -------------------------------------------------------------------------------------

namespace {

// The registry's tables for behaviours: by name, a node's state, the instance's methods.
void ensure_behaviour_tables(KromeModule& s);

// The node a `w` names, from its hidden fields; null when it is gone.
KromeTree* widget_of(lua_State* L, int idx, KromeModule** s, KromeScreen** sc, u32* overlay, u32* node) {
    *s = (KromeModule*)lua_touserdata(L, lua_upvalueindex(1));
    lua_getfield(L, idx, "_s");
    lua_getfield(L, idx, "_o");
    lua_getfield(L, idx, "_n");
    u32 screen = (u32)lua_tointeger(L, -3);
    *overlay = (u32)lua_tointeger(L, -2);
    *node = (u32)lua_tointeger(L, -1);
    lua_pop(L, 3);
    KromeTree* t = krome_target_tree(**s, KromeTarget{screen, *overlay, *node}, sc);
    if (!t || !*sc || *node >= t->nodes.len || (t->nodes[*node].flags & KROME_DEAD)) return nullptr;
    return t;
}

// w:set(v): the bound value written (a number, a string, a boolean), and what reads it told.
int widget_set(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    KromeModule* s;
    KromeScreen* sc;
    u32 overlay, node;
    KromeTree* t = widget_of(L, 1, &s, &sc, &overlay, &node);
    if (!t || t->nodes[node].binding.is_null()) return 0;
    char path[300];
    format(path, sizeof path, "%s", name_str(t->nodes[node].binding));
    lua_pushvalue(L, 2);
    lua_setfield(L, 1, "value");
    if (lua_type(L, 2) == LUA_TTABLE) krome_model_write_value(*s, *sc, path, 2);
    else if (lua_type(L, 2) == LUA_TNUMBER) krome_model_write_number(*s, *sc, path, (f64)lua_tonumber(L, 2));
    else if (lua_type(L, 2) == LUA_TBOOLEAN) {
        // A boolean through the same path as a flip: the store, then what reads it.
        bool want = lua_toboolean(L, 2) != 0;
        if (krome_model_truthy(*s, *sc, path) != want) krome_model_flip(*s, *sc, path);
    } else krome_model_write_string(*s, *sc, path, luaL_tolstring(L, 2, nullptr));
    return 0;
}

// w:emit(event[, value]): as the screen's own event would fire, its handler then KromeEvent.
int widget_emit(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* event = luaL_checkstring(L, 2);
    f64 value = (f64)luaL_optnumber(L, 3, 0.0);
    KromeModule* s;
    KromeScreen* sc;
    u32 overlay, node;
    KromeTree* t = widget_of(L, 1, &s, &sc, &overlay, &node);
    if (t) krome_fire_named(*s, *sc, *t, node, name_intern(event), value);
    return 0;
}

// w:open(tree[, opts]): an overlay anchored to the node (below it, unless opts say).
int widget_open(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    KromeModule* s;
    KromeScreen* sc;
    u32 overlay, node;
    KromeTree* t = widget_of(L, 1, &s, &sc, &overlay, &node);
    if (!t) return 0;
    Name id = t->nodes[node].id;
    open_overlay_at(L, s, sc, 2, lua_istable(L, 3) ? 3 : 0, id);
    if (KromeOverlay* o = krome_overlay(*sc, (u32)lua_tointeger(L, -1))) {
        o->owner = KromeTarget{sc->id, overlay, node};
        if (id.is_null() && !overlay) o->anchor_node = node;
    }
    return 1;
}

// w:part(name, prop, value): a part under the node (`part=fill`) given a property, or a flag
// (a boolean value: set or cleared).
int widget_part(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* part = luaL_checkstring(L, 2);
    const char* prop = luaL_checkstring(L, 3);
    KromeModule* s;
    KromeScreen* sc;
    u32 overlay, node;
    if (!widget_of(L, 1, &s, &sc, &overlay, &node)) return 0;
    KromeTarget t{sc->id, overlay, node};
    if (lua_type(L, 4) == LUA_TBOOLEAN) { krome_set_part(*s, t, part, prop, nullptr, lua_toboolean(L, 4) ? 1 : 0); return 0; }
    KromeValue v;
    char buf[256];
    if (lua_type(L, 4) == LUA_TNUMBER) v = KromeValue::num((f32)lua_tonumber(L, 4));
    else { format(buf, sizeof buf, "%s", luaL_tolstring(L, 4, nullptr)); lua_pop(L, 1); krome_string_value(buf, &v); }
    krome_set_part(*s, t, part, prop, &v, -1);
    return 0;
}

// w:open_slot([style]): the use's children opened below the node; their presses its pick.
int widget_open_slot(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    KromeModule* s;
    KromeScreen* sc;
    u32 overlay, node;
    if (!widget_of(L, 1, &s, &sc, &overlay, &node)) return 0;
    lua_pushinteger(L, (lua_Integer)krome_open_slot(*s, KromeTarget{sc->id, overlay, node}, luaL_optstring(L, 2, nullptr)));
    return 1;
}

// w:close(): what it opened, closed.
int widget_close(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    KromeModule* s;
    KromeScreen* sc;
    u32 overlay, node;
    if (widget_of(L, 1, &s, &sc, &overlay, &node)) krome_close_owned(*s, KromeTarget{sc->id, overlay, node});
    return 0;
}

// w:focus_next(delta): the focus through what is focusable in what it opened.
int widget_focus_next(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    KromeModule* s;
    KromeScreen* sc;
    u32 overlay, node;
    if (widget_of(L, 1, &s, &sc, &overlay, &node)) krome_focus_owned(*s, KromeTarget{sc->id, overlay, node}, (i32)luaL_checkinteger(L, 2));
    return 0;
}

// w:focus(): the focus to the node itself (after what it opened closed).
int widget_focus(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    KromeModule* s;
    KromeScreen* sc;
    u32 overlay, node;
    if (widget_of(L, 1, &s, &sc, &overlay, &node)) krome_focus_take(*s, *sc, overlay, node, lua_toboolean(L, 2) != 0);
    return 0;
}

void ensure_behaviour_tables(KromeModule& s) {
    lua_State* L = s.L;
    if (s.lua_behaviours != LUA_NOREF) return;
    lua_newtable(L);
    s.lua_behaviours = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L);
    s.lua_states = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L);                     // the instance's metatable
    lua_newtable(L);                     // its methods
    const struct { const char* name; lua_CFunction fn; } fns[] = {{"set", widget_set}, {"emit", widget_emit}, {"open", widget_open}, {"part", widget_part},
                                                                  {"open_slot", widget_open_slot}, {"close", widget_close}, {"focus_next", widget_focus_next}, {"focus", widget_focus}};
    for (const auto& f : fns) {
        lua_pushlightuserdata(L, &s);
        lua_pushcclosure(L, f.fn, 1);
        lua_setfield(L, -2, f.name);
    }
    lua_setfield(L, -2, "__index");
    s.lua_widget_meta = luaL_ref(L, LUA_REGISTRYINDEX);
}

// The instance a Lua behaviour gets: { id, rect, lo, hi, step, path, value, focused, state }.
void push_widget(KromeModule& s, KromeScreen& sc, KromeTree& tree, u32 overlay, u32 node) {
    lua_State* L = s.L;
    const KromeNode& n = tree.nodes[node];
    lua_createtable(L, 0, 12);
    if (!n.id.is_null()) { lua_pushstring(L, name_str(n.id)); lua_setfield(L, -2, "id"); }
    lua_createtable(L, 0, 4);
    lua_pushnumber(L, n.rect.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, n.rect.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, n.rect.w); lua_setfield(L, -2, "w");
    lua_pushnumber(L, n.rect.h); lua_setfield(L, -2, "h");
    lua_setfield(L, -2, "rect");
    lua_pushnumber(L, n.lo); lua_setfield(L, -2, "lo");
    lua_pushnumber(L, n.hi); lua_setfield(L, -2, "hi");
    lua_pushnumber(L, n.step); lua_setfield(L, -2, "step");
    lua_pushboolean(L, krome_is_focused(s, sc.id, overlay, node)); lua_setfield(L, -2, "focused");
    if (!n.binding.is_null()) {
        lua_pushstring(L, name_str(n.binding));
        lua_setfield(L, -2, "path");
        push_path(L, sc.store, name_str(n.binding));
        lua_setfield(L, -2, "value");
    }
    // Its state: by its id while it has one, so it lasts a rebuild.
    char key[160];
    if (!n.id.is_null()) format(key, sizeof key, "%u/%s", sc.id, name_str(n.id));
    else format(key, sizeof key, "%u/%u/#%u", sc.id, overlay, node);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s.lua_states);
    if (lua_getfield(L, -1, key) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, key);
    }
    lua_remove(L, -2);
    lua_setfield(L, -2, "state");
    lua_pushinteger(L, sc.id); lua_setfield(L, -2, "_s");
    lua_pushinteger(L, overlay); lua_setfield(L, -2, "_o");
    lua_pushinteger(L, node); lua_setfield(L, -2, "_n");
    lua_rawgeti(L, LUA_REGISTRYINDEX, s.lua_widget_meta);
    lua_setmetatable(L, -2);
}

} // namespace

bool krome_lua_prelude(KromeModule& s, const char* source, usize len, const char* chunk, bool again) {
    lua_State* L = s.L;
    if (!L) return false;
    ensure_behaviour_tables(s);
    char name[300];
    format(name, sizeof name, "@%s", chunk);
    if (luaL_loadbufferx(L, source, len, name, "t") != LUA_OK) {
        krome_error(s, chunk, 0, nullptr, 0, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    // Its own environment over the shared globals, as a screen's.
    lua_newtable(L);
    lua_newtable(L);
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setupvalue(L, -2, 1);
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        krome_error(s, chunk, 0, nullptr, 0, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return true; }
    if (!lua_istable(L, -1)) {
        krome_error(s, chunk, 0, nullptr, 0, "a prelude's script returns its behaviours: { name = { press = function(w, x, y) ... end } }");
        lua_pop(L, 1);
        return false;
    }
    bool ok = true;
    lua_rawgeti(L, LUA_REGISTRYINDEX, s.lua_behaviours); // result registry
    lua_pushnil(L);
    while (lua_next(L, -3)) {            // result registry name calls
        if (lua_type(L, -2) == LUA_TSTRING && lua_istable(L, -1)) {
            const char* bname = lua_tostring(L, -2);
            Name bn = name_intern(bname);
            bool cc = false;
            for (const KromeModule::Behaviour& b : s.behaviours) if (b.name == bn) cc = true;
            lua_getfield(L, -3, bname);
            bool twice = !lua_isnil(L, -1);
            lua_pop(L, 1);
            if (cc || (twice && !again)) {
                krome_error(s, chunk, 0, nullptr, 0, "the behaviour '%s' is registered already%s", bname, cc ? " (from C++)" : "");
                ok = false;
            } else {
                lua_pushvalue(L, -2);
                lua_pushvalue(L, -2);
                lua_rawset(L, -5);
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
    return ok;
}

bool krome_lua_has_behaviour(KromeModule& s, Name name) {
    lua_State* L = s.L;
    if (!L || s.lua_behaviours == LUA_NOREF) return false;
    lua_rawgeti(L, LUA_REGISTRYINDEX, s.lua_behaviours);
    bool has = lua_getfield(L, -1, name_str(name)) == LUA_TTABLE;
    lua_pop(L, 2);
    return has;
}

bool krome_lua_behave(KromeModule& s, KromeScreen& sc, KromeTree& tree, u32 overlay, u32 node, KromeCall call, f32 x, f32 y, const KromeCallArgs& a, bool* taken) {
    lua_State* L = s.L;
    *taken = false;
    if (!L || s.lua_behaviours == LUA_NOREF) return false;
    static const char* const calls[] = {"press", "drag", "release", "wheel", "key", "focus", "hover", "update", "pick"};
    lua_rawgeti(L, LUA_REGISTRYINDEX, s.lua_behaviours);
    if (lua_getfield(L, -1, name_str(tree.nodes[node].behave)) != LUA_TTABLE) { lua_pop(L, 2); return false; }
    if (lua_getfield(L, -1, calls[(u32)call]) != LUA_TFUNCTION) { lua_pop(L, 3); return false; }
    lua_remove(L, -2);
    lua_remove(L, -2);
    push_widget(s, sc, tree, overlay, node);
    int nargs = 1;
    switch (call) {
    case KromeCall::Press:
        lua_pushnumber(L, x); lua_pushnumber(L, y); lua_pushinteger(L, a.button); nargs = 4; break;
    case KromeCall::Drag:
    case KromeCall::Release:
        lua_pushnumber(L, x); lua_pushnumber(L, y); nargs = 3; break;
    case KromeCall::Wheel:
        lua_pushnumber(L, a.dy); nargs = 2; break;
    case KromeCall::Key: {
        char name[32];
        lua_pushstring(L, krome_key_name(a.key, name, sizeof name));
        char mods[32] = {};
        usize m = 0;
        const char* words[] = {"shift", "ctrl", "alt", "cmd"};
        for (u32 i = 0; i < 4; ++i)
            if (a.mods & (1u << i)) m += (usize)format(mods + m, sizeof mods - m, "%s%s", m ? "+" : "", words[i]);
        lua_pushstring(L, mods);
        nargs = 3;
        break;
    }
    case KromeCall::Focus:
    case KromeCall::Hover:
        lua_pushboolean(L, a.on); nargs = 2; break;
    case KromeCall::Update:
        break;
    case KromeCall::Pick:
        lua_pushstring(L, a.id ? a.id : ""); lua_pushstring(L, a.text ? a.text : ""); nargs = 3; break;
    }
    if (lua_pcall(L, nargs, 1, 0) != LUA_OK) {
        KV_ERROR("krome", "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        ++s.script_errors;
        return true;
    }
    *taken = (call == KromeCall::Key || call == KromeCall::Wheel || call == KromeCall::Pick) ? lua_toboolean(L, -1) != 0 : true;
    lua_pop(L, 1);
    return true;
}

void krome_model_write_value(KromeModule& s, KromeScreen& sc, const char* path, int idx) {
    lua_State* L = s.L;
    if (!L || sc.store == LUA_NOREF) return;
    idx = lua_absindex(L, idx);
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
        lua_pushvalue(L, idx);
        bool digits = *last != 0;
        for (const char* q = last; *q; ++q) if (*q < '0' || *q > '9') digits = false;
        if (digits) lua_rawseti(L, -2, (lua_Integer)strtoll(last, nullptr, 10));
        else lua_setfield(L, -2, last);
    }
    lua_pop(L, 1);
    usize head = strcspn(path, ".");
    krome_model_changed(s, sc, name_intern(path, head).hash);
}

bool krome_model_number(KromeModule& s, KromeScreen& sc, const char* path, f64* out) {
    lua_State* L = s.L;
    if (!L || sc.store == LUA_NOREF) return false;
    push_path(L, sc.store, path);
    bool ok = true;
    if (lua_type(L, -1) == LUA_TNUMBER) *out = (f64)lua_tonumber(L, -1);
    else if (lua_type(L, -1) == LUA_TBOOLEAN) *out = lua_toboolean(L, -1) ? 1.0 : 0.0;
    else if (lua_type(L, -1) == LUA_TSTRING) {
        char* end = nullptr;
        const char* t = lua_tostring(L, -1);
        f64 v = strtod(t, &end);
        ok = end != t && *end == 0;
        if (ok) *out = v;
    } else ok = false;
    lua_pop(L, 1);
    return ok;
}

void krome_model_write_number(KromeModule& s, KromeScreen& sc, const char* path, f64 v) {
    lua_State* L = s.L;
    if (!L || sc.store == LUA_NOREF) return;
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
        if (v == (f64)(i64)v && v < 9e15 && v > -9e15) lua_pushinteger(L, (lua_Integer)v);
        else lua_pushnumber(L, (lua_Number)v);
        bool digits = *last != 0;
        for (const char* q = last; *q; ++q) if (*q < '0' || *q > '9') digits = false;
        if (digits) lua_rawseti(L, -2, (lua_Integer)strtoll(last, nullptr, 10));
        else lua_setfield(L, -2, last);
    }
    lua_pop(L, 1);
    usize head = strcspn(path, ".");
    krome_model_changed(s, sc, name_intern(path, head).hash);
}

} // namespace kv
