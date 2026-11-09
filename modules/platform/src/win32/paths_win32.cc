// The executable's directory from GetModuleFileNameW; the preference directory under the
// roaming application data folder (%APPDATA%/org/app/), the same place SDL puts it. Paths
// come back with '/' separators, as the engine writes them everywhere else.
#include "../os_paths.h"
#include "korvid/platform/io.h"
#include "korvid/core/log.h"
#include "wide.h"
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace kv {

bool os_base_path(char* out, u32 cap) {
    static wchar_t w[32768];
    DWORD n = GetModuleFileNameW(nullptr, w, (DWORD)(sizeof w / sizeof w[0]));
    if (n == 0 || n >= sizeof w / sizeof w[0]) return false;
    if (!narrow(w, out, (int)cap)) return false;
    forward_slashes(out);
    char* slash = strrchr(out, '/');
    if (!slash) return false;
    slash[1] = 0;
    return true;
}

bool os_pref_path(const char* org, const char* app, char* out, u32 cap) {
    PWSTR dir = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &dir))) {
        KV_WARN("platform", "no preference path: Windows has no roaming application data folder for this user");
        return false;
    }
    char base[1024];
    bool ok = narrow(dir, base, (int)sizeof base);
    CoTaskMemFree(dir);
    if (!ok) { KV_WARN("platform", "no preference path: too long"); return false; }
    forward_slashes(base);
    int n = snprintf(out, cap, "%s/%s/%s/", base, org, app);
    if (n <= 0 || (u32)n >= cap) { KV_WARN("platform", "no preference path: too long"); return false; }
    if (!io_make_dirs(out)) { KV_WARN("platform", "no preference path: cannot create %s", out); return false; }
    return true;
}

} // namespace kv
