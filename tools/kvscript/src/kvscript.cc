// kvscript: the module. One command, `kvscript run SCRIPT [ARGS...]`: a Lua state with the
// standard libraries and `kv` (image, font, zip, fs, json, sha256, random), the script read through the
// platform's blocking I/O and run to its end, inside the command, on the main thread; then
// the tool finishes with the script's status. `arg` holds the script (arg[0]) and its
// arguments, which also come to the chunk as `...`. A script that returns a number exits
// with it; an error exits 1 with its message and traceback; anything else, 0.
//
// kv.image and kv.font are kvscript's own (image.cc, font.cc); the rest of `kv` is the
// script module's (modules/script), which goldberg opens too.
#include "kvscript.h"
#include "korvid/kvscript/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "korvid/core/module.h"
#include "korvid/platform/io.h"
#include "korvid/script/script.h"
#include "korvid/tool/tool.h"
#include <string.h>

namespace kv {

namespace {

// ---- running a script ----------------------------------------------------------------------

int run_script(const char* path, Span<const char* const> args) {
    IoBytes source;
    i32 err = 0;
    if (!io_read_now(path, &source, nullptr, &err)) { print_err("kvscript: cannot read '%s' (errno %d)\n", path, err); return 1; }
    lua_State* L = kv_lua_newstate(mem_general());
    if (!L) { source.free(); print_err("kvscript: no memory for Lua\n"); return 1; }
    luaL_openlibs(L);
    script_open_kv(L); // fs, zip, json, sha256, random
    kvscript_open_image(L);
    lua_setfield(L, -2, "image");
    kvscript_open_font(L);
    lua_setfield(L, -2, "font");
    kvscript_open_svg(L);
    lua_setfield(L, -2, "svg");
    kvscript_open_patherizer(L);
    lua_setfield(L, -2, "patherizer");
    lua_setglobal(L, "kv");
    lua_createtable(L, (int)args.len, 1);
    lua_pushstring(L, path);
    lua_rawseti(L, -2, 0);
    for (u32 i = 0; i < args.len; ++i) {
        lua_pushstring(L, args[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setglobal(L, "arg");
    lua_pushcfunction(L, script_traceback);
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

KV_MODULE_DEFINE(kvscript_module) = {
    .name     = "kvscript",
    .commands = k_commands,
};

} // namespace kv
