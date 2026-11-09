/* kormake: the host. A Lua 5.5 state, the embedded scripts, and the few things the
 * scripts need from the operating system: the file system, the environment, running a
 * command. The operating system itself is behind os.h (os_posix.c, os_win32.c). Module
 * files and the master file are evaluated by the scripts in a sandbox that sees none of
 * this. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "os.h"
#include "scripts.inc"

#define KORMAKE_VERSION "1.0"

/* The platform this kormake runs on: what "host" means in a target or a module's platforms. */
#if defined(__linux__)
#define KORMAKE_HOST "linux"
#elif defined(__APPLE__)
#define KORMAKE_HOST "macos"
#elif defined(_WIN32)
#define KORMAKE_HOST "windows"
#else
#error "kormake: unknown host platform"
#endif

#define KORMAKE_PATH_MAX 4096

typedef struct { lua_State* L; int n; } ListState;

static void list_one(void* user, const char* name) {
    ListState* s = (ListState*)user;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return;
    lua_pushstring(s->L, name);
    lua_rawseti(s->L, -2, ++s->n);
}

static int kfs_listdir(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    ListState s = { L, 0 };
    lua_newtable(L);
    kos_listdir(path, list_one, &s);
    return 1;
}

static int kfs_isdir(lua_State* L) {
    KosStat st;
    lua_pushboolean(L, kos_stat(luaL_checkstring(L, 1), &st) == 0 && st.is_dir);
    return 1;
}

static int kfs_isfile(lua_State* L) {
    KosStat st;
    lua_pushboolean(L, kos_stat(luaL_checkstring(L, 1), &st) == 0 && st.is_file);
    return 1;
}

static int kfs_mtime(lua_State* L) {
    KosStat st;
    if (kos_stat(luaL_checkstring(L, 1), &st) != 0) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, (lua_Integer)st.mtime);
    return 1;
}

static int kfs_read(lua_State* L) {
    FILE* f = kos_fopen(luaL_checkstring(L, 1), "rb");
    if (!f) { lua_pushnil(L); lua_pushstring(L, kos_error()); return 2; }
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) luaL_addlstring(&b, chunk, n);
    fclose(f);
    luaL_pushresult(&b);
    return 1;
}

/* Writes only if the content changed, so Ninja does not see a touched file. With a true
 * third argument an unchanged file is touched instead: build.ninja must be newer than the
 * inputs of its regenerate rule even when an edit to one of them changed nothing, or Ninja
 * regenerates it forever. */
static int kfs_write(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    size_t len;
    const char* text = luaL_checklstring(L, 2, &len);
    int touch = lua_toboolean(L, 3);
    FILE* f = kos_fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long have = ftell(f);
        int same = have == (long)len;
        if (same) {
            fseek(f, 0, SEEK_SET);
            char* buf = (char*)malloc(len ? len : 1);
            same = fread(buf, 1, len, f) == len && memcmp(buf, text, len) == 0;
            free(buf);
        }
        fclose(f);
        if (same) {
            if (touch) kos_touch(path);
            lua_pushboolean(L, 0);
            return 1;
        }
    }
    f = kos_fopen(path, "wb");
    if (!f) return luaL_error(L, "cannot write '%s': %s", path, kos_error());
    fwrite(text, 1, len, f);
    fclose(f);
    lua_pushboolean(L, 1);
    return 1;
}

static int kfs_mkdirp(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    char buf[KORMAKE_PATH_MAX];
    snprintf(buf, sizeof buf, "%s", path);
    for (char* p = buf + 1; *p; ++p) {
        if (!KOS_IS_SEP(*p)) continue;
        char sep = *p;
        *p = 0;
        if (kos_mkdir(buf) != 0) return luaL_error(L, "cannot create '%s': %s", buf, kos_error());
        *p = sep;
    }
    if (kos_mkdir(buf) != 0) return luaL_error(L, "cannot create '%s': %s", buf, kos_error());
    return 0;
}

static int kfs_rmtree(lua_State* L) {
    lua_pushboolean(L, kos_rmtree(luaL_checkstring(L, 1)) == 0);
    return 1;
}

/* copy(from, to): replaces to; an executable stays executable. */
static int kfs_copy(lua_State* L) {
    const char* from = luaL_checkstring(L, 1);
    const char* to = luaL_checkstring(L, 2);
    if (kos_copy(from, to) != 0) return luaL_error(L, "cannot copy '%s' to '%s': %s", from, to, kos_error());
    return 0;
}

static int kfs_cwd(lua_State* L) {
    char buf[KORMAKE_PATH_MAX];
    if (kos_cwd(buf, sizeof buf) != 0) return luaL_error(L, "cannot read the current directory: %s", kos_error());
    lua_pushstring(L, buf);
    return 1;
}

static int kfs_realpath(lua_State* L) {
    char buf[KORMAKE_PATH_MAX];
    if (kos_realpath(luaL_checkstring(L, 1), buf, sizeof buf) != 0) { lua_pushnil(L); return 1; }
    lua_pushstring(L, buf);
    return 1;
}

static int kfs_getenv(lua_State* L) {
    char buf[32768];
    if (kos_getenv(luaL_checkstring(L, 1), buf, sizeof buf)) lua_pushstring(L, buf); else lua_pushnil(L);
    return 1;
}

static int kfs_exec(lua_State* L) {
    lua_pushinteger(L, kos_exec(luaL_checkstring(L, 1)));
    return 1;
}

/* The executable's own path, for the regenerate rule. */
static int kfs_self(lua_State* L) {
    char buf[KORMAKE_PATH_MAX];
    if (kos_self(buf, sizeof buf) != 0) { lua_pushnil(L); return 1; }
    lua_pushstring(L, buf);
    return 1;
}

static const luaL_Reg kfs[] = {
    {"listdir", kfs_listdir}, {"isdir", kfs_isdir}, {"isfile", kfs_isfile}, {"mtime", kfs_mtime},
    {"read", kfs_read}, {"write", kfs_write}, {"mkdirp", kfs_mkdirp}, {"rmtree", kfs_rmtree},
    {"copy", kfs_copy}, {"cwd", kfs_cwd}, {"realpath", kfs_realpath}, {"getenv", kfs_getenv},
    {"exec", kfs_exec}, {"self", kfs_self}, {0, 0}
};

/* kormake_script(name): the embedded script's source, for the loader in main.lua. */
static int script_source(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    for (int i = 0; kormake_scripts[i].name; ++i)
        if (strcmp(kormake_scripts[i].name, name) == 0) { lua_pushstring(L, kormake_scripts[i].text); return 1; }
    lua_pushnil(L);
    return 1;
}

int kormake_main(int argc, char** argv) {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    luaL_newlib(L, kfs);
    lua_setglobal(L, "kfs");
    lua_pushcfunction(L, script_source);
    lua_setglobal(L, "kormake_script");
    lua_pushstring(L, KORMAKE_VERSION);
    lua_setglobal(L, "KORMAKE_VERSION");
    lua_pushstring(L, KORMAKE_HOST);
    lua_setglobal(L, "KORMAKE_HOST");
    lua_newtable(L);
    for (int i = 1; i < argc; ++i) { lua_pushstring(L, argv[i]); lua_rawseti(L, -2, i); }
    lua_setglobal(L, "arg");

    const char* main_src = 0;
    for (int i = 0; kormake_scripts[i].name; ++i) if (strcmp(kormake_scripts[i].name, "main") == 0) main_src = kormake_scripts[i].text;
    if (!main_src) { fprintf(stderr, "kormake: main script missing\n"); return 1; }
    int rc = 1;
    if (luaL_loadbuffer(L, main_src, strlen(main_src), "=kormake/main.lua") != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
        fprintf(stderr, "kormake: %s\n", lua_tostring(L, -1));
    } else {
        rc = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : 0;
    }
    lua_close(L);
    return rc;
}
