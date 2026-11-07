/* kormake: the host. A Lua 5.5 state, the embedded scripts, and the few things the
 * scripts need from the operating system: the file system, the environment, running a
 * command. Module files and the master file are evaluated by the scripts in a sandbox
 * that sees none of this. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "scripts.inc"

#define KORMAKE_VERSION "1.0"

static int kfs_listdir(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    DIR* d = opendir(path);
    lua_newtable(L);
    if (!d) return 1;
    struct dirent* e;
    int n = 0;
    while ((e = readdir(d)) != 0) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        lua_pushstring(L, e->d_name);
        lua_rawseti(L, -2, ++n);
    }
    closedir(d);
    return 1;
}

static int kfs_isdir(lua_State* L) {
    struct stat st;
    lua_pushboolean(L, stat(luaL_checkstring(L, 1), &st) == 0 && S_ISDIR(st.st_mode));
    return 1;
}

static int kfs_isfile(lua_State* L) {
    struct stat st;
    lua_pushboolean(L, stat(luaL_checkstring(L, 1), &st) == 0 && S_ISREG(st.st_mode));
    return 1;
}

static int kfs_mtime(lua_State* L) {
    struct stat st;
    if (stat(luaL_checkstring(L, 1), &st) != 0) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, (lua_Integer)st.st_mtime);
    return 1;
}

static int kfs_read(lua_State* L) {
    FILE* f = fopen(luaL_checkstring(L, 1), "rb");
    if (!f) { lua_pushnil(L); lua_pushstring(L, strerror(errno)); return 2; }
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) luaL_addlstring(&b, chunk, n);
    fclose(f);
    luaL_pushresult(&b);
    return 1;
}

/* Writes only if the content changed, so Ninja does not see a touched file. */
static int kfs_write(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    size_t len;
    const char* text = luaL_checklstring(L, 2, &len);
    FILE* f = fopen(path, "rb");
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
        if (same) { lua_pushboolean(L, 0); return 1; }
    }
    f = fopen(path, "wb");
    if (!f) return luaL_error(L, "cannot write '%s': %s", path, strerror(errno));
    fwrite(text, 1, len, f);
    fclose(f);
    lua_pushboolean(L, 1);
    return 1;
}

static int kfs_mkdirp(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", path);
    for (char* p = buf + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(buf, 0775) != 0 && errno != EEXIST) return luaL_error(L, "cannot create '%s': %s", buf, strerror(errno));
        *p = '/';
    }
    if (mkdir(buf, 0775) != 0 && errno != EEXIST) return luaL_error(L, "cannot create '%s': %s", buf, strerror(errno));
    return 0;
}

static int rmtree(const char* path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path);
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != 0) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                char child[4096];
                snprintf(child, sizeof child, "%s/%s", path, e->d_name);
                rmtree(child);
            }
            closedir(d);
        }
        return rmdir(path);
    }
    return unlink(path);
}

static int kfs_rmtree(lua_State* L) {
    lua_pushboolean(L, rmtree(luaL_checkstring(L, 1)) == 0);
    return 1;
}

static int kfs_cwd(lua_State* L) {
    char buf[4096];
    if (!getcwd(buf, sizeof buf)) return luaL_error(L, "getcwd failed");
    lua_pushstring(L, buf);
    return 1;
}

static int kfs_realpath(lua_State* L) {
    char buf[4096];
    if (!realpath(luaL_checkstring(L, 1), buf)) { lua_pushnil(L); return 1; }
    lua_pushstring(L, buf);
    return 1;
}

static int kfs_getenv(lua_State* L) {
    const char* v = getenv(luaL_checkstring(L, 1));
    if (v) lua_pushstring(L, v); else lua_pushnil(L);
    return 1;
}

static int kfs_exec(lua_State* L) {
    int rc = system(luaL_checkstring(L, 1));
    lua_pushinteger(L, rc == -1 ? -1 : (rc >> 8) & 0xFF);
    return 1;
}

/* The executable's own path, for the regenerate rule. */
static int kfs_self(lua_State* L) {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n < 0) { lua_pushnil(L); return 1; }
    buf[n] = 0;
    lua_pushstring(L, buf);
    return 1;
}

static const luaL_Reg kfs[] = {
    {"listdir", kfs_listdir}, {"isdir", kfs_isdir}, {"isfile", kfs_isfile}, {"mtime", kfs_mtime},
    {"read", kfs_read}, {"write", kfs_write}, {"mkdirp", kfs_mkdirp}, {"rmtree", kfs_rmtree},
    {"cwd", kfs_cwd}, {"realpath", kfs_realpath}, {"getenv", kfs_getenv}, {"exec", kfs_exec},
    {"self", kfs_self}, {0, 0}
};

/* kormake_script(name): the embedded script's source, for the loader in main.lua. */
static int script_source(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    for (int i = 0; kormake_scripts[i].name; ++i)
        if (strcmp(kormake_scripts[i].name, name) == 0) { lua_pushstring(L, kormake_scripts[i].text); return 1; }
    lua_pushnil(L);
    return 1;
}

int main(int argc, char** argv) {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    luaL_newlib(L, kfs);
    lua_setglobal(L, "kfs");
    lua_pushcfunction(L, script_source);
    lua_setglobal(L, "kormake_script");
    lua_pushstring(L, KORMAKE_VERSION);
    lua_setglobal(L, "KORMAKE_VERSION");
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
