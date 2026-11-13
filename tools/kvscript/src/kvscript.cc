// kvscript: the module. One command, `kvscript run SCRIPT [ARGS...]`: a Lua state with the
// standard libraries and `kv` (image, font, zip, fs, json, sha256, random), the script read through the
// platform's blocking I/O and run to its end, inside the command, on the main thread; then
// the tool finishes with the script's status. `arg` holds the script (arg[0]) and its
// arguments, which also come to the chunk as `...`. A script that returns a number exits
// with it; an error exits 1 with its message and traceback; anything else, 0.
//
// kv.fs and kv.zip are here too: files and directories through the platform (UTF-8 paths
// on every OS, which Lua's io library cannot promise on Windows), and zips read whole
// through the pack module's reader and the inflate module.
#include "kvscript.h"
#include "korvid/kvscript/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "korvid/core/module.h"
#include "korvid/core/random.h"
#include "korvid/inflate/inflate.h"
#include "korvid/pack/zip.h"
#include "korvid/platform/io.h"
#include "korvid/tool/tool.h"
#include <string.h>

namespace kv {

namespace {

// ---- kv.fs ------------------------------------------------------------------------------

int fs_read(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    IoBytes file;
    i32 err = 0;
    if (!io_read_now(path, &file, nullptr, &err)) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot read '%s' (errno %d)", path, (int)err);
        return 2;
    }
    lua_pushlstring(L, (const char*)file.data, file.size); // can raise only for memory: then the bytes are lost
    file.free();
    return 1;
}
int fs_write(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    usize n = 0;
    const char* data = luaL_checklstring(L, 2, &n);
    i32 err = 0;
    if (!io_write_now(path, data, n, &err)) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot write '%s' (errno %d)", path, (int)err);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}
int fs_is_file(lua_State* L) { lua_pushboolean(L, io_is_file(luaL_checkstring(L, 1))); return 1; }
int fs_is_dir(lua_State* L) { lua_pushboolean(L, io_is_dir(luaL_checkstring(L, 1))); return 1; }
int fs_exists(lua_State* L) {
    const char* p = luaL_checkstring(L, 1);
    lua_pushboolean(L, io_is_file(p) || io_is_dir(p));
    return 1;
}
int fs_mkdir(lua_State* L) { lua_pushboolean(L, io_make_dirs(luaL_checkstring(L, 1))); return 1; }
int fs_remove(lua_State* L) { lua_pushboolean(L, io_remove(luaL_checkstring(L, 1))); return 1; }
int fs_remove_tree(lua_State* L) { lua_pushboolean(L, io_remove_tree(luaL_checkstring(L, 1))); return 1; }
int fs_temp_dir(lua_State* L) {
    char out[512];
    if (!io_make_temp_dir(luaL_optstring(L, 1, "kvscript"), out, sizeof out)) return luaL_error(L, "fs: cannot make a temporary directory");
    lua_pushstring(L, out);
    return 1;
}
struct ListState { lua_State* L; lua_Integer n; };
bool list_one(void* user, const char* relative) {
    ListState* s = (ListState*)user;
    lua_pushstring(s->L, relative);
    lua_rawseti(s->L, -2, ++s->n);
    return true;
}
// Every file under dir, recursively, as paths relative to it, sorted per directory; hidden
// ones left out. nil and a message if dir cannot be read.
int fs_list(lua_State* L) {
    const char* dir = luaL_checkstring(L, 1);
    lua_newtable(L);
    ListState s{L, 0};
    if (io_list_files(dir, list_one, &s) < 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot list '%s'", dir);
        return 2;
    }
    return 1;
}
constexpr luaL_Reg k_fs[] = {
    {"read", fs_read},     {"write", fs_write},   {"is_file", fs_is_file},         {"is_dir", fs_is_dir},
    {"exists", fs_exists}, {"mkdir", fs_mkdir},   {"remove", fs_remove},           {"remove_tree", fs_remove_tree},
    {"list", fs_list},     {"temp_dir", fs_temp_dir}, {nullptr, nullptr},
};

// ---- kv.zip -------------------------------------------------------------------------------

constexpr const char* kZipMeta = "kv.zip";

// The archive's bytes and its central directory; freed by the collector.
struct Zip {
    IoBytes   file;
    PackTable table;
    bool      open;
};
int zip_gc(lua_State* L) {
    Zip* z = (Zip*)luaL_checkudata(L, 1, kZipMeta);
    if (z->open) { pack_table_free(z->table, mem_general()); z->file.free(); z->open = false; }
    return 0;
}
Zip* zip_check(lua_State* L, int i) {
    Zip* z = (Zip*)luaL_checkudata(L, i, kZipMeta);
    if (!z->open) luaL_error(L, "zip: closed");
    return z;
}

int zip_open(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    Zip* z = (Zip*)lua_newuserdatauv(L, sizeof(Zip), 0);
    memset((void*)z, 0, sizeof *z);
    luaL_setmetatable(L, kZipMeta); // from here the collector frees what is opened
    i32 err = 0;
    if (!io_read_now(path, &z->file, nullptr, &err)) return luaL_error(L, "zip: cannot read '%s' (errno %d)", path, (int)err);
    z->open = true;
    const char* why = nullptr;
    ZipTail tail;
    usize n = z->file.size, tail_len = n < 65557 ? n : 65557;
    if (!zip_find_end(z->file.data + n - tail_len, tail_len, n - tail_len, &tail, &why)) return luaL_error(L, "zip: '%s': %s", path, why);
    if (tail.needs_zip64) {
        if (tail.zip64_end + 56 > n || !zip_read_zip64_end(z->file.data + tail.zip64_end, 56, &tail, &why))
            return luaL_error(L, "zip: '%s': %s", path, why ? why : "a Zip64 end record past the file");
    }
    if (tail.cd_offset + tail.cd_size > n) return luaL_error(L, "zip: '%s': the central directory runs past the file", path);
    if (!zip_read_central(z->file.data + tail.cd_offset, (usize)tail.cd_size, tail, mem_general(), &z->table, &why))
        return luaL_error(L, "zip: '%s': %s", path, why);
    return 1;
}

// Every entry's name, in the archive's order (by where each one's data is), as Python's
// namelist gives them: "the first that ends in ..." is then the same entry.
int zip_names(lua_State* L) {
    Zip* z = zip_check(L, 1);
    u32 n = z->table.count;
    // An index sorted by header offset, in a userdata so an error loses nothing.
    u32* order = (u32*)lua_newuserdatauv(L, sizeof(u32) * (n ? n : 1), 0);
    for (u32 i = 0; i < n; ++i) order[i] = i;
    for (u32 i = 1; i < n; ++i) { // insertion sort: archives hold hundreds, not millions
        u32 v = order[i];
        u64 key = z->table.entries[v].header_offset;
        u32 j = i;
        for (; j > 0 && z->table.entries[order[j - 1]].header_offset > key; --j) order[j] = order[j - 1];
        order[j] = v;
    }
    lua_createtable(L, (int)n, 0);
    for (u32 i = 0; i < n; ++i) {
        lua_pushstring(L, pack_entry_name(z->table, z->table.entries[order[i]]));
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 1;
}

const PackEntry* zip_entry(Zip* z, const char* name) {
    for (u32 i = 0; i < z->table.count; ++i)
        if (!strcmp(pack_entry_name(z->table, z->table.entries[i]), name)) return &z->table.entries[i];
    return nullptr;
}

// An entry's bytes, inflated if they were deflated, checked against its CRC.
int zip_read(lua_State* L) {
    Zip* z = zip_check(L, 1);
    const char* name = luaL_checkstring(L, 2);
    const PackEntry* e = zip_entry(z, name);
    if (!e) return luaL_error(L, "zip: no '%s'", name);
    u64 data = 0;
    usize need = 0;
    usize n = z->file.size;
    if (e->header_offset >= n ||
        !zip_local_data_offset(z->file.data + e->header_offset, n - (usize)e->header_offset, e->header_offset, &data, &need) ||
        data + e->csize > n)
        return luaL_error(L, "zip: '%s': its data is past the file", name);
    const u8* src = z->file.data + data;
    if (e->method == ZipStored) {
        if (e->csize != e->size) return luaL_error(L, "zip: '%s': stored, but its sizes differ", name);
        if (crc32(0, src, (usize)e->size) != e->crc) return luaL_error(L, "zip: '%s': bad CRC", name);
        lua_pushlstring(L, (const char*)src, (usize)e->size);
        return 1;
    }
    if (e->method != ZipDeflate) return luaL_error(L, "zip: '%s': method %d, neither stored nor deflated", name, (int)e->method);
    // Inflated into a userdata, so an error after it loses nothing.
    u8* out = (u8*)lua_newuserdatauv(L, (usize)e->size + 1, 0);
    InflateOut r = inflate_raw(src, (usize)e->csize, out, (usize)e->size);
    if (r.result != InflateResult::Ok || r.produced != e->size) return luaL_error(L, "zip: '%s': %s", name, inflate_result_name(r.result));
    if (crc32(0, out, (usize)e->size) != e->crc) return luaL_error(L, "zip: '%s': bad CRC", name);
    lua_pushlstring(L, (const char*)out, (usize)e->size);
    return 1;
}

int zip_has(lua_State* L) {
    Zip* z = zip_check(L, 1);
    lua_pushboolean(L, zip_entry(z, luaL_checkstring(L, 2)) != nullptr);
    return 1;
}

constexpr luaL_Reg k_zip_methods[] = {{"names", zip_names}, {"read", zip_read}, {"has", zip_has}, {nullptr, nullptr}};
constexpr luaL_Reg k_zip[] = {{"open", zip_open}, {nullptr, nullptr}};

// ---- kv.random --------------------------------------------------------------------------------
// kv.random(seed): the engine's Mt19937 seeded as Python's random.Random(seed), with Python's
// names, so a tool ported from Python draws the same numbers: r:random(), r:randint(a, b),
// r:randrange(n), r:choice(list), r:getrandbits32().

constexpr const char* kRandomMeta = "kv.random";

Mt19937* random_check(lua_State* L) { return (Mt19937*)luaL_checkudata(L, 1, kRandomMeta); }
int random_new(lua_State* L) {
    lua_Integer seed = luaL_checkinteger(L, 1);
    if (seed < 0) seed = -seed; // Python seeds from abs(seed)
    Mt19937* m = (Mt19937*)lua_newuserdatauv(L, sizeof(Mt19937), 0);
    *m = Mt19937::seeded((u64)seed);
    luaL_setmetatable(L, kRandomMeta);
    return 1;
}
int random_random(lua_State* L) { lua_pushnumber(L, random_check(L)->unit()); return 1; }
int random_bits(lua_State* L) { lua_pushinteger(L, random_check(L)->next()); return 1; }
int random_randrange(lua_State* L) {
    Mt19937* m = random_check(L);
    lua_Integer n = luaL_checkinteger(L, 2);
    if (n <= 0 || n > 0xffffffffll) return luaL_error(L, "random: randrange(%d): 1 to 2^32 - 1", (int)n);
    lua_pushinteger(L, m->below((u32)n));
    return 1;
}
int random_randint(lua_State* L) {
    Mt19937* m = random_check(L);
    lua_Integer a = luaL_checkinteger(L, 2), b = luaL_checkinteger(L, 3);
    if (b < a || b - a >= 0xffffffffll) return luaL_error(L, "random: randint(%d, %d)", (int)a, (int)b);
    lua_pushinteger(L, m->range(a, b));
    return 1;
}
int random_choice(lua_State* L) {
    Mt19937* m = random_check(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_Integer n = luaL_len(L, 2);
    if (n <= 0) return luaL_error(L, "random: choice of an empty list");
    lua_geti(L, 2, (lua_Integer)m->below((u32)n) + 1);
    return 1;
}
constexpr luaL_Reg k_random_methods[] = {
    {"random", random_random}, {"randint", random_randint}, {"randrange", random_randrange},
    {"choice", random_choice}, {"getrandbits32", random_bits}, {nullptr, nullptr},
};

// ---- running a script ----------------------------------------------------------------------

int traceback(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    if (!msg) msg = lua_pushfstring(L, "(an error object that is a %s)", luaL_typename(L, 1));
    luaL_traceback(L, L, msg, 1);
    return 1;
}

int run_script(const char* path, Span<const char* const> args) {
    IoBytes source;
    i32 err = 0;
    if (!io_read_now(path, &source, nullptr, &err)) { print_err("kvscript: cannot read '%s' (errno %d)\n", path, err); return 1; }
    lua_State* L = luaL_newstate();
    if (!L) { source.free(); print_err("kvscript: no memory for Lua\n"); return 1; }
    luaL_openlibs(L);
    lua_newtable(L); // kv
    kvscript_open_image(L);
    lua_setfield(L, -2, "image");
    kvscript_open_font(L);
    lua_setfield(L, -2, "font");
    kvscript_open_zip(L);
    lua_setfield(L, -2, "zip");
    kvscript_open_fs(L);
    lua_setfield(L, -2, "fs");
    kvscript_open_json_sha(L);
    luaL_newmetatable(L, kRandomMeta);
    lua_newtable(L);
    luaL_setfuncs(L, k_random_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    lua_pushcfunction(L, random_new);
    lua_setfield(L, -2, "random");
    lua_setglobal(L, "kv");
    lua_createtable(L, (int)args.len, 1);
    lua_pushstring(L, path);
    lua_rawseti(L, -2, 0);
    for (u32 i = 0; i < args.len; ++i) {
        lua_pushstring(L, args[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setglobal(L, "arg");
    lua_pushcfunction(L, traceback);
    char chunk[600];
    format(chunk, sizeof chunk, "@%s", path);
    int status = luaL_loadbuffer(L, (const char*)source.data, source.size, chunk);
    source.free();
    int code = 0;
    if (status == LUA_OK) {
        for (u32 i = 0; i < args.len; ++i) lua_pushstring(L, args[i]);
        status = lua_pcall(L, (int)args.len, 1, 1);
    }
    if (status != LUA_OK) {
        print_err("kvscript: %s\n", lua_tostring(L, -1));
        code = 1;
    } else if (lua_isinteger(L, -1)) {
        code = (int)lua_tointeger(L, -1);
    }
    print_flush();
    lua_close(L);
    return code;
}

void cmd_run(CliContext& ctx, Span<const char* const> args) {
    if (args.len < 1) { ctx.fail("kvscript run <script.lua> [args...]: runs the script with the engine's libraries"); return; }
    int code = run_script(args[0], Span<const char* const>{args.data + 1, args.len - 1});
    tool_finish(*ctx.engine, code);
}

constinit const CliCommandDesc k_commands[] = {
    {.name = "kvscript run", .help = "kvscript run <script.lua> [args...]: Lua with kv.image, kv.font, kv.zip and kv.fs; exits with the script's status",
     .fn = cmd_run},
};

} // namespace

void kvscript_open_fs(lua_State* L) {
    lua_newtable(L);
    luaL_setfuncs(L, k_fs, 0);
}

void kvscript_open_zip(lua_State* L) {
    luaL_newmetatable(L, kZipMeta);
    lua_pushcfunction(L, zip_gc);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    luaL_setfuncs(L, k_zip_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    lua_newtable(L);
    luaL_setfuncs(L, k_zip, 0);
}

KV_MODULE_DEFINE(kvscript_module) = {
    .name     = "kvscript",
    .commands = k_commands,
};

} // namespace kv
