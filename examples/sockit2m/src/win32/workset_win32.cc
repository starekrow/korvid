// sockit2m's working set on Windows. A unique directory by process id and a counter; the
// writes flushed file by file (FlushFileBuffers; Windows has no sync()); the page cache
// dropped by opening each file without buffering. Windows has no unprivileged call that
// evicts one file's pages, but the cache manager flushes and purges a file's cached data
// when a handle opens it with FILE_FLAG_NO_BUFFERING, so the next read through an ordinary
// handle meets the disk. measured, not assumed: the run's cold and hot passes show it.
#include "../torture.h"
#include "korvid/core/format.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <errno.h>
#include <string.h>

namespace kv {

namespace {

// UTF-8 to UTF-16, the engine's paths to Win32's. False if it does not fit.
bool workset_widen(const char* s, wchar_t* out, int cap) { return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, cap) > 0; }

i32 workset_errno(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return ENOENT;
    case ERROR_ACCESS_DENIED:                             return EACCES;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS:    return EEXIST;
    default:                                              return EIO;
    }
}

// Opens path with the given access and flags, does nothing, closes it. False if it would not open.
bool touch_handle(const char* path, DWORD access, DWORD flags, bool flush) {
    wchar_t w[1024];
    if (!workset_widen(path, w, 1024)) return false;
    HANDLE h = CreateFileW(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = !flush || FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

} // namespace

bool workset_make_unique_dir(char* path, i32* err) {
    usize n = strlen(path);
    if (n < 6 || strcmp(path + n - 6, "XXXXXX") != 0) { *err = EINVAL; return false; }
    static u32 counter = 0;
    for (u32 tries = 0; tries < 100; ++tries) {
        char six[8];
        format(six, sizeof six, "%06x", (GetCurrentProcessId() * 31u + ++counter * 7919u) & 0xFFFFFFu);
        memcpy(path + n - 6, six, 6);
        wchar_t w[1024];
        if (!workset_widen(path, w, 1024)) { *err = ENAMETOOLONG; return false; }
        if (CreateDirectoryW(w, nullptr)) return true;
        if (GetLastError() != ERROR_ALREADY_EXISTS) { *err = workset_errno(GetLastError()); return false; }
    }
    *err = EEXIST;
    return false;
}

void workset_flush() {
    for (u32 i = 0; i < g_opt.files; ++i) {
        char path[600];
        file_path(path, sizeof path, i);
        touch_handle(path, GENERIC_WRITE, FILE_ATTRIBUTE_NORMAL, true);
    }
}

bool evict_working_set() {
    bool ok = true;
    for (u32 i = 0; i < g_opt.files; ++i) {
        char path[600];
        file_path(path, sizeof path, i);
        // Flushed first (a purge drops clean pages only), then opened uncached: the purge.
        if (!touch_handle(path, GENERIC_WRITE, FILE_ATTRIBUTE_NORMAL, true)) ok = false;
        if (!touch_handle(path, GENERIC_READ, FILE_FLAG_NO_BUFFERING, false)) ok = false;
    }
    return ok;
}

void remove_working_set() {
    for (u32 i = 0; i < g_opt.files; ++i) {
        char path[600];
        file_path(path, sizeof path, i);
        wchar_t w[1024];
        if (workset_widen(path, w, 1024)) DeleteFileW(w);
    }
    wchar_t w[1024];
    if (workset_widen(g_work_dir, w, 1024)) RemoveDirectoryW(w);
}

} // namespace kv
