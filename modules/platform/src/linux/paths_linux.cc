// The executable's directory from /proc/self/exe; the preference directory under
// $XDG_DATA_HOME (or ~/.local/share), the same place SDL put it.
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

bool os_pref_path(const char* org, const char* app, char* out, u32 cap) {
    const char* xdg  = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    int n;
    if (xdg && xdg[0] == '/') n = format(out, cap, "%s/%s/%s/", xdg, org, app);
    else if (home && home[0]) n = format(out, cap, "%s/.local/share/%s/%s/", home, org, app);
    else { KV_WARN("platform", "no preference path: neither XDG_DATA_HOME nor HOME is set"); return false; }
    if (n <= 0 || (u32)n >= cap) { KV_WARN("platform", "no preference path: too long"); return false; }
    if (!io_make_dirs(out)) { KV_WARN("platform", "no preference path: cannot create %s", out); return false; }
    return true;
}

} // namespace kv
