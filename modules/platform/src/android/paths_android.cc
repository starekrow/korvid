// The directories of an Android process. An app (platform_android_app was called): its
// files are inside its package, which os_bundle_archive names and the mounts and
// io_read_bundled read; there is no base path; the preference directory is under the app's
// private files directory. A test under adb's shell is a Linux program: the executable's
// directory from /proc/self/exe, the preference directory under TMPDIR (the shell has no
// home it can write).
#include "../os_paths.h"
#include "korvid/platform/android.h"
#include "korvid/platform/io.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace kv {

namespace {

char g_package[512];
char g_internal[512];
pthread_mutex_t g_fg_lock = PTHREAD_MUTEX_INITIALIZER;
void (*g_fg_listener)(bool) = nullptr;
bool g_foreground = true;

} // namespace

void platform_android_app(const char* package, const char* internal) {
    format(g_package, sizeof g_package, "%s", package ? package : "");
    format(g_internal, sizeof g_internal, "%s", internal ? internal : "");
}

void platform_android_foreground(bool foreground) {
    pthread_mutex_lock(&g_fg_lock);
    g_foreground = foreground;
    void (*fn)(bool) = g_fg_listener;
    pthread_mutex_unlock(&g_fg_lock);
    if (fn) fn(foreground);
}

void platform_android_on_foreground(void (*fn)(bool foreground)) {
    pthread_mutex_lock(&g_fg_lock);
    g_fg_listener = fn;
    bool now = g_foreground;
    pthread_mutex_unlock(&g_fg_lock);
    if (fn && !now) fn(false); // opened while away: starts when the app is back
}

const char* os_bundle_archive() { return g_package[0] ? g_package : nullptr; }

bool os_base_path(char* out, u32 cap) {
    if (cap < 2 || g_package[0]) return false; // an app has no directory of its own files
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0 || (size_t)n >= cap - 1) return false;
    out[n] = 0;
    char* slash = strrchr(out, '/');
    if (!slash) return false;
    slash[1] = 0;
    return true;
}

bool os_exe_name(char* out, u32 cap) {
    if (g_package[0]) return false; // an app: its files are in its package
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

bool os_pref_path(const char* org, const char* app, char* out, u32 cap) {
    const char* root = g_internal[0] ? g_internal : getenv("TMPDIR");
    if (!root || !root[0]) { KV_WARN("platform", "no preference path: not an app, and TMPDIR is not set"); return false; }
    int n = format(out, cap, "%s/%s/%s/", root, org, app);
    if (n <= 0 || (u32)n >= cap) { KV_WARN("platform", "no preference path: too long"); return false; }
    if (!io_make_dirs(out)) { KV_WARN("platform", "no preference path: cannot create %s", out); return false; }
    return true;
}

} // namespace kv
