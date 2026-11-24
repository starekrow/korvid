// kvscript: shapes (kv.svg, kv.patherizer; docs/PRDs/Krome-Slice-5-PRD.md). An asset script can
// read an SVG or a .kshape and draw it at any size into a kv.image:
//
//   local shape, messages = kv.svg.load("icons/check.svg")      -- or kv.svg.parse(text)
//   shape:draw(64, 64, {0, 0, 0, 255}):save("check_64.png")     -- currentColor, optional
//   kv.fs.write("check.kshape", shape:encode())
//   local again = kv.patherizer.read(kv.fs.read("check.kshape"))
#include "kvscript.h"
#include "korvid/core/memory.h"
#include "korvid/patherizer/patherizer.h"
#include "korvid/platform/io.h"
#include "korvid/svg/svg.h"
#include <new>
#include <string.h>

namespace kv {

// Named: kvscript is a unity build, and image.cc's anonymous namespace has a check() too.
namespace kvs_shape {

constexpr const char* kShapeMeta = "kv.shape";

PathShape* push_shape(lua_State* L) {
    PathShape* s = (PathShape*)lua_newuserdatauv(L, sizeof(PathShape), 0);
    new (s) PathShape();
    s->init(mem_general());
    luaL_setmetatable(L, kShapeMeta);
    return s;
}

PathShape* check(lua_State* L, int i) { return (PathShape*)luaL_checkudata(L, i, kShapeMeta); }

int l_gc(lua_State* L) { check(L, 1)->free(); return 0; }

// The SVG text into a shape: the shape (or nil) and a list of messages ("line: text").
int convert(lua_State* L, const char* text, usize len, const char* name, int opts) {
    SvgOptions o;
    if (lua_istable(L, opts)) { lua_getfield(L, opts, "lenient"); o.lenient = lua_toboolean(L, -1); lua_pop(L, 1); }
    PathShape* s = push_shape(L);
    SvgMessages m;
    bool ok = svg_to_shape(text, len, o, *s, m);
    lua_createtable(L, (int)m.items.len, 0);
    for (u32 i = 0; i < m.items.len; ++i) {
        char line[300];
        m.format_one(i, name, line, sizeof line);
        lua_pushstring(L, line);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    m.free();
    if (!ok) { lua_pushnil(L); lua_replace(L, -3); }
    return 2;
}

int l_svg_parse(lua_State* L) {
    usize len = 0;
    const char* text = luaL_checklstring(L, 1, &len);
    return convert(L, text, len, "svg", 2);
}

int l_svg_load(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    IoBytes b;
    if (!io_read_file(path, &b)) return luaL_error(L, "kv.svg.load: cannot read '%s'", path);
    int n = convert(L, (const char*)b.data, b.size, path, 2);
    b.free();
    return n;
}

int l_read(lua_State* L) {
    usize len = 0;
    const char* bytes = luaL_checklstring(L, 1, &len);
    PathShape* s = push_shape(L);
    const char* why = "";
    if (!patherizer_read_kshape(bytes, len, *s, &why)) { lua_pushnil(L); lua_pushstring(L, why); return 2; }
    return 1;
}

// shape:draw(w, h [, {r, g, b, a}]) -> a kv.image, straight alpha.
int l_draw(lua_State* L) {
    PathShape* s = check(L, 1);
    i32 w = (i32)luaL_checkinteger(L, 2), h = (i32)luaL_checkinteger(L, 3);
    PathOptions o;
    if (lua_istable(L, 4))
        for (int k = 0; k < 4; ++k) { lua_rawgeti(L, 4, k + 1); o.current[k] = (u8)luaL_optinteger(L, -1, 255); lua_pop(L, 1); }
    Image* m = kvscript_push_image(L, w, h);
    PathTarget t{image_px(m), (u32)w, (u32)h, (u32)w * 4, false};
    if (!patherizer_draw(*s, t, o)) return luaL_error(L, "shape:draw: out of memory");
    return 1;
}

int l_encode(lua_State* L) {
    PathShape* s = check(L, 1);
    Array<u8> out;
    out.alloc = mem_general();
    if (!patherizer_write_kshape(*s, out, lua_isnone(L, 2) ? true : lua_toboolean(L, 2))) { out.free(); return luaL_error(L, "shape:encode failed"); }
    lua_pushlstring(L, (const char*)out.data, out.len);
    out.free();
    return 1;
}

int l_view(lua_State* L) {
    PathShape* s = check(L, 1);
    for (int k = 0; k < 4; ++k) lua_pushnumber(L, s->view[k]);
    return 4;
}

int l_one_colour(lua_State* L) { lua_pushboolean(L, check(L, 1)->one_colour()); return 1; }

const luaL_Reg k_methods[] = {{"draw", l_draw}, {"encode", l_encode}, {"view", l_view}, {"one_colour", l_one_colour}, {nullptr, nullptr}};
const luaL_Reg k_svg[] = {{"load", l_svg_load}, {"parse", l_svg_parse}, {nullptr, nullptr}};
const luaL_Reg k_patherizer[] = {{"read", l_read}, {nullptr, nullptr}};

void meta(lua_State* L) {
    if (luaL_newmetatable(L, kShapeMeta)) {
        lua_newtable(L);
        luaL_setfuncs(L, k_methods, 0);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
}

} // namespace kvs_shape

using namespace kvs_shape;

void kvscript_open_svg(lua_State* L) {
    meta(L);
    lua_newtable(L);
    luaL_setfuncs(L, k_svg, 0);
}

void kvscript_open_patherizer(lua_State* L) {
    meta(L);
    lua_newtable(L);
    luaL_setfuncs(L, k_patherizer, 0);
}

} // namespace kv
