// The executable's directory from /proc/self/exe; the user's directories under the XDG
// base directories (the data one is where SDL put its preference path).
#include "../os_paths.h"
#include "korvid/platform/io.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace kv {

bool os_base_path(char* out, u32 cap) {
    if (cap < 2) return false;
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0 || (size_t)n >= cap - 1) return false;
    out[n] = 0;
    char* slash = strrchr(out, '/');
    if (!slash) return false;
    slash[1] = 0;
    return true;
}

bool os_exe_name(char* out, u32 cap) {
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) return false;
    path[n] = 0;
    const char* slash = strrchr(path, '/');
    const char* name = slash ? slash + 1 : path;
    if (!name[0] || strlen(name) >= cap) return false;
    memcpy(out, name, strlen(name) + 1);
    return true;
}

// The XDG base directories: the variable when it is an absolute path (the spec says to
// ignore a relative one), else its default under $HOME.
bool os_user_dir(UserDir which, const char* org, const char* app, char* out, u32 cap) {
    const char* var  = which == UserDir::Settings ? "XDG_CONFIG_HOME" : which == UserDir::Cache ? "XDG_CACHE_HOME" : "XDG_DATA_HOME";
    const char* dflt = which == UserDir::Settings ? ".config" : which == UserDir::Cache ? ".cache" : ".local/share";
    const char* xdg  = getenv(var);
    const char* home = getenv("HOME");
    int n;
    if (xdg && xdg[0] == '/') n = format(out, cap, "%s/%s/%s/", xdg, org, app);
    else if (home && home[0]) n = format(out, cap, "%s/%s/%s/%s/", home, dflt, org, app);
    else { KV_WARN("platform", "no preference path: neither %s nor HOME is set", var); return false; }
    if (n <= 0 || (u32)n >= cap) { KV_WARN("platform", "no preference path: too long"); return false; }
    if (!io_make_dirs(out)) { KV_WARN("platform", "no preference path: cannot create %s", out); return false; }
    return true;
}

} // namespace kv
