// The user's three directories (KR24): settings, data and cache, each made under
// <org>/<app>/. On Linux, the XDG base directories when set to absolute paths, their
// defaults under $HOME otherwise, a relative one ignored as the spec says.
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/test.h"
#include <stdlib.h>
#include <string.h>

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
        KV_CHECK(c, !strcmp(platform_pref_path(e, "Org", "App"), want)); // the data directory
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
#endif
}
