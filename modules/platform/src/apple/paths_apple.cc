// The Apple family's directories, macOS and iOS alike: the executable's directory from dyld;
// the preference directory under ~/Library/Application Support/<org>/<app>/, the same place
// SDL puts it (on iOS, HOME is the app's container, so this is inside it). Inside a macOS
// .app (kormake package) the executable is in Contents/MacOS/ and what sits beside it
// elsewhere (the pack, shaders/, assets/) is in Contents/Resources/: the base path is that,
// when it is there. An iOS app is flat, its resources beside the executable, which is the
// rule everywhere else. Plain POSIX and dyld, no Foundation: nothing here needs NSBundle.
#include "../os_paths.h"
#include "korvid/platform/io.h"
#include "korvid/core/log.h"
#include <limits.h>
#include <mach-o/dyld.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    // In a bundle: <app>/Contents/MacOS/ -> <app>/Contents/Resources/, if the bundle has one.
    const char* macos = "/Contents/MacOS/";
    usize len = strlen(out), tail = strlen(macos);
    if (len >= tail && strcmp(out + len - tail, macos) == 0) {
        char resources[PATH_MAX];
        int r = snprintf(resources, sizeof resources, "%.*s/Contents/Resources/", (int)(len - tail), out);
        struct stat st;
        if (r > 0 && (u32)r < cap && stat(resources, &st) == 0 && S_ISDIR(st.st_mode)) memcpy(out, resources, (usize)r + 1);
    }
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
