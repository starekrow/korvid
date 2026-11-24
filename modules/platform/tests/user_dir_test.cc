// The user's three directories (KR24): settings, data and cache, each made under
// <org>/<app>/. On Linux, the XDG base directories when set to absolute paths, their
// defaults under $HOME otherwise, a relative one ignored as the spec says. On macOS,
// settings and data together in ~/Library/Application Support, the cache in ~/Library/Caches.
// On Windows, settings and data in %APPDATA%, the cache in %LOCALAPPDATA%, in cache/.
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

void test_user_dirs(kv::TestCtx& c) {
    using namespace kv;
    c.begin("user dirs");
#if defined(__linux__)
    char root[200], want[400];
    KV_CHECK(c, io_make_temp_dir("korvid-dirs", root, sizeof root));
    const char* saved_home = getenv("HOME");
    char home_copy[512] = {};
    if (saved_home) format(home_copy, sizeof home_copy, "%s", saved_home);
    const char* vars[] = {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"};
    char saved[3][512] = {};
    bool had[3] = {};
    for (u32 i = 0; i < 3; ++i) if (const char* v = getenv(vars[i])) { had[i] = true; format(saved[i], sizeof saved[i], "%s", v); }

    const ModuleDesc* const list[] = {&platform_module};
    // Each set: those places.
    {
        char p[300];
        format(p, sizeof p, "%s/conf", root); setenv("XDG_CONFIG_HOME", p, 1);
        format(p, sizeof p, "%s/data", root); setenv("XDG_DATA_HOME", p, 1);
        format(p, sizeof p, "%s/cache", root); setenv("XDG_CACHE_HOME", p, 1);
        Engine e;
        KV_CHECK(c, engine_init(e, list));
        format(want, sizeof want, "%s/conf/Org/App/", root);
        KV_CHECK(c, !strcmp(platform_user_dir(e, UserDir::Settings, "Org", "App"), want) && io_is_dir(want));
        format(want, sizeof want, "%s/data/Org/App/", root);
        KV_CHECK(c, !strcmp(platform_user_dir(e, UserDir::Data, "Org", "App"), want) && io_is_dir(want));
        KV_CHECK(c, !strcmp(platform_user_dir(e, UserDir::Data, "Org", "App"), want)); // the data directory
        format(want, sizeof want, "%s/cache/Org/App/", root);
        KV_CHECK(c, !strcmp(platform_user_dir(e, UserDir::Cache, "Org", "App"), want) && io_is_dir(want));
        engine_shutdown(e);
    }
    // None set, or one relative: the defaults under $HOME.
    {
        setenv("HOME", root, 1);
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_DATA_HOME");
        setenv("XDG_CACHE_HOME", "relative/cache", 1);
        Engine e;
        KV_CHECK(c, engine_init(e, list));
        format(want, sizeof want, "%s/.config/Org/App/", root);
        KV_CHECK(c, !strcmp(platform_user_dir(e, UserDir::Settings, "Org", "App"), want) && io_is_dir(want));
        format(want, sizeof want, "%s/.local/share/Org/App/", root);
        KV_CHECK(c, !strcmp(platform_user_dir(e, UserDir::Data, "Org", "App"), want));
        format(want, sizeof want, "%s/.cache/Org/App/", root);
        KV_CHECK(c, !strcmp(platform_user_dir(e, UserDir::Cache, "Org", "App"), want) && io_is_dir(want));
        engine_shutdown(e);
    }
    if (home_copy[0]) setenv("HOME", home_copy, 1);
    for (u32 i = 0; i < 3; ++i) { if (had[i]) setenv(vars[i], saved[i], 1); else unsetenv(vars[i]); }
    KV_CHECK(c, io_remove_tree(root));
#elif defined(__APPLE__) && TARGET_OS_OSX
    char root[200], want[400];
    KV_CHECK(c, io_make_temp_dir("korvid-dirs", root, sizeof root));
    char home_copy[512] = {};
    if (const char* h = getenv("HOME")) format(home_copy, sizeof home_copy, "%s", h);
    setenv("HOME", root, 1);
    {
        const ModuleDesc* const list[] = {&platform_module};
        Engine e;
        KV_CHECK(c, engine_init(e, list));
        format(want, sizeof want, "%s/Library/Application Support/Org/App/", root);
        KV_CHECK(c, !strcmp(platform_user_dir(e, UserDir::Settings, "Org", "App"), want) && io_is_dir(want));
        KV_CHECK(c, !strcmp(platform_user_dir(e, UserDir::Data, "Org", "App"), want));
        KV_CHECK(c, !strcmp(platform_user_dir(e, UserDir::Data, "Org", "App"), want)); // the data directory
        format(want, sizeof want, "%s/Library/Caches/Org/App/", root);
        KV_CHECK(c, !strcmp(platform_user_dir(e, UserDir::Cache, "Org", "App"), want) && io_is_dir(want));
        engine_shutdown(e);
    }
    if (home_copy[0]) setenv("HOME", home_copy, 1); else unsetenv("HOME");
    KV_CHECK(c, io_remove_tree(root));
#elif defined(_WIN32)
    // The known folders are the user's own (no variable moves them), so the test makes its
    // directories there, under an org of its own, and takes them away after.
    auto slashed = [](const char* var, char* out, u32 cap) {
        const char* v = getenv(var);
        if (!v || !*v) return false;
        format(out, cap, "%s", v);
        for (char* p = out; *p; ++p) if (*p == '\\') *p = '/';
        return true;
    };
    char roaming[400], local[400], org[64], want[600];
    KV_CHECK(c, slashed("APPDATA", roaming, sizeof roaming) && slashed("LOCALAPPDATA", local, sizeof local));
    format(org, sizeof org, "korvid-dirs-test-%llx", (unsigned long long)(time_now() * 1e6));
    {
        const ModuleDesc* const list[] = {&platform_module};
        Engine e;
        KV_CHECK(c, engine_init(e, list));
        format(want, sizeof want, "%s/%s/App/", roaming, org);
        KV_CHECK(c, !strcmp(platform_user_dir(e, UserDir::Settings, org, "App"), want) && io_is_dir(want));
        KV_CHECK(c, !strcmp(platform_user_dir(e, UserDir::Data, org, "App"), want));
        KV_CHECK(c, !strcmp(platform_user_dir(e, UserDir::Data, org, "App"), want)); // the data directory
        format(want, sizeof want, "%s/%s/App/cache/", local, org);
        KV_CHECK(c, !strcmp(platform_user_dir(e, UserDir::Cache, org, "App"), want) && io_is_dir(want));
        engine_shutdown(e);
    }
    format(want, sizeof want, "%s/%s", roaming, org);
    KV_CHECK(c, io_remove_tree(want));
    format(want, sizeof want, "%s/%s", local, org);
    KV_CHECK(c, io_remove_tree(want));
#endif
}
