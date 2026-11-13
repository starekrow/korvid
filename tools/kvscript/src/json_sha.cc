// kvscript: kv.sha256 (FIPS 180-4, in C: a pack is megabytes) and kv.json (in Lua, below:
// writing JSON is string work, which Lua does well). kv.json writes what Python's json.dump
// writes, so a tool ported from Python makes the same file: objects in the order their keys
// were set (kv.json.object), floats as Python's repr, strings escaped to ASCII, and
// Python's two layouts, `indent` (",\n" between items, ": " after keys) or compact
// separators.
#include "kvscript.h"
#include <string.h>

namespace kv {

namespace {

// ---- SHA-256 -----------------------------------------------------------------------------

constexpr u32 kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

u32 rotr(u32 x, u32 n) { return (x >> n) | (x << (32 - n)); }

void block(u32 h[8], const u8* p) {
    u32 w[64];
    for (u32 i = 0; i < 16; ++i) w[i] = (u32)p[i * 4] << 24 | (u32)p[i * 4 + 1] << 16 | (u32)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (u32 i = 16; i < 64; ++i) {
        u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (u32 i = 0; i < 64; ++i) {
        u32 t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
        u32 t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

// kv.sha256(bytes): the digest as 64 lowercase hex digits.
int l_sha256(lua_State* L) {
    usize n = 0;
    const u8* p = (const u8*)luaL_checklstring(L, 1, &n);
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    usize full = n / 64 * 64;
    for (usize i = 0; i < full; i += 64) block(h, p + i);
    u8 tail[128] = {};
    usize rest = n - full;
    memcpy(tail, p + full, rest);
    tail[rest] = 0x80;
    usize len = rest + 1 + 8 <= 64 ? 64 : 128;
    u64 bits = (u64)n * 8;
    for (u32 i = 0; i < 8; ++i) tail[len - 1 - i] = (u8)(bits >> (8 * i));
    block(h, tail);
    if (len == 128) block(h, tail + 64);
    char hex[65];
    for (u32 i = 0; i < 8; ++i)
        for (u32 j = 0; j < 8; ++j) hex[i * 8 + j] = "0123456789abcdef"[(h[i] >> (28 - 4 * j)) & 15];
    lua_pushlstring(L, hex, 64);
    return 1;
}

// ---- kv.json ---------------------------------------------------------------------------------

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
return json
)lua";

} // namespace

// Sets kv.sha256 and kv.json on the table at the top of the stack.
void kvscript_open_json_sha(lua_State* L) {
    lua_pushcfunction(L, l_sha256);
    lua_setfield(L, -2, "sha256");
    if (luaL_loadbuffer(L, kJson, sizeof kJson - 1, "=kv.json") != LUA_OK) lua_error(L);
    lua_call(L, 0, 1);
    lua_setfield(L, -2, "json");
}

} // namespace kv
