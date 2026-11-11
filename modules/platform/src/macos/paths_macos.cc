// The executable's directory from dyld; the preference directory under
// ~/Library/Application Support/<org>/<app>/, the same place SDL puts it on macOS. Inside
// an .app the executable's directory is Contents/MacOS/; finding the bundle's Resources/
// from it is the packager's milestone (slice 7, M6), shared with iOS through src/apple/.
#include "../os_paths.h"
#include "korvid/platform/io.h"
#include "korvid/core/log.h"
#include <limits.h>
#include <mach-o/dyld.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace kv {

bool os_base_path(char* out, u32 cap) {
    if (cap < 2) return false;
    // dyld reports the path the process was started by, which may hold symlinks or "./";
    // realpath makes it the file's own.
    char raw[PATH_MAX];
    uint32_t raw_cap = sizeof raw;
    if (_NSGetExecutablePath(raw, &raw_cap) != 0) return false;
    char real[PATH_MAX];
    if (!realpath(raw, real)) return false;
    usize n = strlen(real);
    if (n >= cap) return false;
    memcpy(out, real, n + 1);
    char* slash = strrchr(out, '/');
    if (!slash) return false;
    slash[1] = 0;
    return true;
}

bool os_pref_path(const char* org, const char* app, char* out, u32 cap) {
    const char* home = getenv("HOME");
    if (!home || !home[0]) {
        const passwd* pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : nullptr;
    }
    if (!home || !home[0]) { KV_WARN("platform", "no preference path: no home directory"); return false; }
    int n = snprintf(out, cap, "%s/Library/Application Support/%s/%s/", home, org, app);
    if (n <= 0 || (u32)n >= cap) { KV_WARN("platform", "no preference path: too long"); return false; }
    if (!io_make_dirs(out)) { KV_WARN("platform", "no preference path: cannot create %s", out); return false; }
    return true;
}

} // namespace kv
