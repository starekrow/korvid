// kv.json. Writing is in Lua, below (string work, which Lua does well): what Python's
// json.dump writes, so a tool ported from Python makes the same file: objects in the order
// their keys were set (kv.json.object), floats as Python's repr, strings escaped to ASCII,
// and Python's two layouts, `indent` (",\n" between items, ": " after keys) or compact
// separators. Reading is the engine's parser (korvid/core/json.h) turned into the same
// values, with the line each one was written on kept beside it.
#include "korvid/script/script.h"
#include "korvid/core/arena.h"
#include "korvid/core/json.h"
#include <string.h>

namespace kv {

namespace {

constexpr const char kJson[] = R"lua(
local json = {}
local KEYS, VALS = setmetatable({}, { __mode = "k" }), setmetatable({}, { __mode = "k" })
local Object = { __name = "kv.json.object" }
Object.__index = function(o, k) return VALS[o][k] end
Object.__newindex = function(o, k, v)
    local vals = VALS[o]
    if vals[k] == nil then local keys = KEYS[o]; keys[#keys + 1] = k end
    vals[k] = v
end
Object.__pairs = function(o)
    local keys, vals, i = KEYS[o], VALS[o], 0
    return function() i = i + 1; local k = keys[i]; if k ~= nil then return k, vals[k] end end
end
-- An object whose keys come out in the order they were first set: object("a", 1, "b", 2).
function json.object(...)
    local o = setmetatable({}, Object)
    KEYS[o], VALS[o] = {}, {}
    for i = 1, select("#", ...), 2 do local k, v = select(i, ...); o[k] = v end
    return o
end
-- json.null is null; a plain table is an array (an empty one is []).
json.null = setmetatable({}, { __name = "kv.json.null" })

-- Python's repr of a float: the shortest digits that read back, fixed notation from 1e-4
-- to under 1e16, scientific otherwise, and ".0" on a whole number.
local function repr(v)
    if v ~= v then return "NaN" end
    if v == math.huge then return "Infinity" end
    if v == -math.huge then return "-Infinity" end
    if v == 0 then return 1 / v < 0 and "-0.0" or "0.0" end
    local s
    for p = 0, 16 do
        s = string.format("%." .. p .. "e", v)
        if tonumber(s) == v then break end
    end
    local sign, d1, rest, e = s:match("^(%-?)(%d)%.?(%d*)e([%-+]%d+)$")
    local digits = (d1 .. rest):gsub("0+$", "")
    if digits == "" then digits = "0" end
    local exp = tonumber(e)
    if exp < -4 or exp >= 16 then
        local m = digits:sub(1, 1) .. (#digits > 1 and ("." .. digits:sub(2)) or "")
        return string.format("%s%se%s%02d", sign, m, exp < 0 and "-" or "+", math.abs(exp))
    end
    if exp < 0 then return sign .. "0." .. string.rep("0", -exp - 1) .. digits end
    if #digits <= exp + 1 then return sign .. digits .. string.rep("0", exp + 1 - #digits) .. ".0" end
    return sign .. digits:sub(1, exp + 1) .. "." .. digits:sub(exp + 2)
end
json.repr = repr

local escapes = { ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }
local function quote(s)
    local out = { '"' }
    for _, c in utf8.codes(s) do
        local ch = utf8.char(c)
        if escapes[ch] then out[#out + 1] = escapes[ch]
        elseif c < 0x20 then out[#out + 1] = string.format("\\u%04x", c)
        elseif c < 0x80 then out[#out + 1] = ch
        elseif c < 0x10000 then out[#out + 1] = string.format("\\u%04x", c)
        else
            c = c - 0x10000
            out[#out + 1] = string.format("\\u%04x\\u%04x", 0xd800 + (c >> 10), 0xdc00 + (c & 0x3ff))
        end
    end
    out[#out + 1] = '"'
    return table.concat(out)
end

-- encode(value [, options]): options.indent (a count of spaces, or nil for one line) and
-- options.separators ({item, key}; Python's defaults: {",", ": "} with an indent, {", ",
-- ": "} without).
function json.encode(value, options)
    options = options or {}
    local indent = options.indent
    local sep = options.separators or (indent and { ",", ": " } or { ", ", ": " })
    local out = {}
    local function emit(v, level)
        local t = type(v)
        if v == json.null or v == nil then out[#out + 1] = "null"
        elseif t == "boolean" then out[#out + 1] = v and "true" or "false"
        elseif t == "number" then out[#out + 1] = math.type(v) == "integer" and tostring(v) or repr(v)
        elseif t == "string" then out[#out + 1] = quote(v)
        elseif t == "table" then
            local is_object = getmetatable(v) == Object
            local items = {}
            if is_object then for k, x in pairs(v) do items[#items + 1] = { k, x } end
            else for i = 1, #v do items[i] = { nil, v[i] } end end
            if #items == 0 then out[#out + 1] = is_object and "{}" or "[]"; return end
            out[#out + 1] = is_object and "{" or "["
            local pad = indent and ("\n" .. string.rep(" ", indent * (level + 1))) or nil
            for i, it in ipairs(items) do
                if i > 1 then out[#out + 1] = sep[1] end
                if pad then out[#out + 1] = pad end
                if is_object then out[#out + 1] = quote(tostring(it[1])); out[#out + 1] = sep[2] end
                emit(it[2], level + 1)
            end
            if pad then out[#out + 1] = "\n" .. string.rep(" ", indent * level) end
            out[#out + 1] = is_object and "}" or "]"
        else
            error("json: cannot write a " .. t)
        end
    end
    emit(value, 0)
    return table.concat(out)
end

-- Reading. json.decode(text [, options]) is set from C (json.cc); what it builds is what
-- encode writes: objects as json.object (their keys in the order written), arrays as
-- plain tables, null as json.null. It also remembers where each thing was written:
-- json.line(t) is the line a decoded object or array began on, json.line(t, key) the
-- line of one of its members or items.
local LINES = setmetatable({}, { __mode = "k" })
function json._note(t, own, lines) LINES[t] = { own, lines } end
function json.line(t, key)
    local l = LINES[t]
    if not l then return nil end
    if key == nil then return l[1] end
    return l[2][key]
end
function json.is_object(v) return getmetatable(v) == Object end
-- An object's keys, in order, as a new list.
function json.keys(o)
    local out = {}
    for i, k in ipairs(KEYS[o]) do out[i] = k end
    return out
end
return json
)lua";

struct Decode {
    lua_State*     L;
    const JsonDoc* doc;
    int            json;  // the module's table
    const char*    drop;  // a member name to leave out, or null
};

// Pushes v as a Lua value.
void decode_value(Decode& d, const JsonValue& v) {
    lua_State* L = d.L;
    luaL_checkstack(L, 8, "json: nested too deep");
    switch (v.type) {
    case JsonType::Null:   lua_getfield(L, d.json, "null"); return;
    case JsonType::Bool:   lua_pushboolean(L, v.number() != 0.0); return;
    case JsonType::String: lua_pushlstring(L, v.str(), v.len()); return;
    case JsonType::Bytes:  lua_pushlstring(L, (const char*)v.bytes(), v.len()); return; // KBOR's: a Lua string holds bytes
    case JsonType::Number:
        // An integer as written stays one when Lua can hold it.
        if (v.integer() && (v.number() < 0.0 || v.bits() <= 0x7FFFFFFFFFFFFFFFull)) lua_pushinteger(L, (lua_Integer)v.bits());
        else lua_pushnumber(L, v.number());
        return;
    case JsonType::Array: {
        lua_createtable(L, (int)v.count(), 0);
        int arr = lua_gettop(L);
        lua_createtable(L, (int)v.count(), 0);
        int lines = lua_gettop(L);
        lua_Integer i = 0;
        for (const JsonValue* c = json_first(*d.doc, v); c; c = json_next(*d.doc, *c)) {
            decode_value(d, *c);
            lua_rawseti(L, arr, ++i);
            lua_pushinteger(L, c->line);
            lua_rawseti(L, lines, i);
        }
        lua_getfield(L, d.json, "_note");
        lua_pushvalue(L, arr);
        lua_pushinteger(L, v.line);
        lua_pushvalue(L, lines);
        lua_call(L, 3, 0);
        lua_pop(L, 1); // lines
        return;
    }
    case JsonType::Object: {
        lua_getfield(L, d.json, "object");
        lua_call(L, 0, 1);
        int obj = lua_gettop(L);
        lua_createtable(L, 0, (int)v.count());
        int lines = lua_gettop(L);
        for (const JsonValue* c = json_first(*d.doc, v); c; c = json_next(*d.doc, *c)) {
            if (d.drop && strcmp(c->key, d.drop) == 0) continue;
            decode_value(d, *c);
            lua_setfield(L, obj, c->key); // through the object's __newindex: a repeated name keeps its first place and its last value
            lua_pushinteger(L, c->line);
            lua_setfield(L, lines, c->key);
        }
        lua_getfield(L, d.json, "_note");
        lua_pushvalue(L, obj);
        lua_pushinteger(L, v.line);
        lua_pushvalue(L, lines);
        lua_call(L, 3, 0);
        lua_pop(L, 1);
        return;
    }
    }
    lua_pushnil(L);
}

// json.decode(text [, options]): the value; or nil, a message and the line it is about.
// options.comments allows `//` to the end of a line; options.drop names a member left out
// wherever it appears (a "//" key used as a comment).
int l_decode(lua_State* L) {
    usize n = 0;
    const char* text = luaL_checklstring(L, 1, &n);
    u32 flags = 0;
    const char* drop = nullptr;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "comments");
        if (lua_toboolean(L, -1)) flags |= KV_JSON_LINE_COMMENTS;
        lua_getfield(L, 2, "drop");
        drop = lua_tostring(L, -1); // stays on the stack, so the string stays alive
    }
    // The parse lives in the scratch arena, marked and released by hand: a Lua error below
    // (memory, only) leaves this frame by longjmp, and nothing with a destructor may be
    // alive across that. What it leaves marked, the next release above reclaims.
    Arena* arena = mem_scratch_arena();
    usize mark = arena_mark(*arena);
    JsonDoc doc;
    if (!json_parse(text, n, mem_scratch(), doc, flags)) {
        lua_pushnil(L);
        lua_pushstring(L, doc.error);
        lua_pushinteger(L, doc.error_line);
        arena_release(*arena, mark);
        return 3;
    }
    Decode d{L, &doc, lua_upvalueindex(1), drop};
    decode_value(d, doc.root());
    arena_release(*arena, mark);
    return 1;
}

} // namespace

void script_open_json(lua_State* L) {
    if (luaL_loadbuffer(L, kJson, sizeof kJson - 1, "=kv.json") != LUA_OK) lua_error(L);
    lua_call(L, 0, 1);
    lua_pushvalue(L, -1);
    lua_pushcclosure(L, l_decode, 1);
    lua_setfield(L, -2, "decode");
}

} // namespace kv
