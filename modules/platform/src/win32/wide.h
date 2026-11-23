// Korvid platform on Windows: the edge between the engine's UTF-8 and the wide (UTF-16)
// Win32 API, and Win32 errors as the errno values the rest of the engine speaks. Every
// Windows source in this module includes it; inline, so a unity build sees it once.
// Strings cross as WTF-8 (wtf8.h), UTF-8 that can also carry a lone surrogate, so a file
// whose UTF-16 name is not valid Unicode lists and opens like any other.
#pragma once
#include "korvid/core/types.h"
#include "../wtf8.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <errno.h>
#include <string.h>

namespace kv {

static_assert(sizeof(wchar_t) == sizeof(u16), "wchar_t is UTF-16 on Windows");

// A path's room in UTF-16: the engine's own path buffers are at most 1024 bytes.
constexpr int KV_WIDE_PATH_MAX = 1024;

// A Win32 error as the errno value the engine speaks.
inline i32 errno_from_win32(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME:
    case ERROR_BAD_NETPATH: case ERROR_BAD_PATHNAME: case ERROR_INVALID_DRIVE:       return ENOENT;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: return EACCES;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS:                                 return EEXIST;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY:                              return ENOMEM;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL:                                 return ENOSPC;
    case ERROR_FILENAME_EXCED_RANGE: case ERROR_BUFFER_OVERFLOW:                       return ENAMETOOLONG;
    case ERROR_DIRECTORY:                                                              return ENOTDIR;
    case ERROR_TOO_MANY_OPEN_FILES:                                                    return EMFILE;
    case ERROR_WRITE_PROTECT:                                                          return EROFS;
    case ERROR_INVALID_PARAMETER: case ERROR_INVALID_HANDLE:                           return EINVAL;
    case ERROR_DIR_NOT_EMPTY:                                                          return ENOTEMPTY;
    default:                                                                           return EIO;
    }
}

// WTF-8 into out (cap characters, terminated). False, with *err = ENAMETOOLONG or EILSEQ,
// if it does not fit or is not WTF-8.
inline bool widen(const char* s, wchar_t* out, int cap, i32* err = nullptr) {
    int n = utf16_from_wtf8(s, (u16*)out, cap);
    if (n > 0) return true;
    if (err) *err = n == 0 ? ENAMETOOLONG : EILSEQ;
    return false;
}

// UTF-16 into out as WTF-8 (cap bytes, terminated): UTF-8 unless the string holds a lone
// surrogate. False if it does not fit.
inline bool narrow(const wchar_t* w, char* out, int cap) {
    return wtf8_from_utf16((const u16*)w, out, cap) > 0;
}

// A path as Win32's file calls take it, of any length: widened, and past MAX_PATH made
// absolute, with backslashes, under the \\?\ prefix (\\?\UNC\ for a share), which lifts
// the limit and turns off the API's own parsing, so GetFullPathNameW resolves . and .. first.
// A short path costs nothing new: it stays in `small`, as widen would leave it. On failure
// `w` is null and *err (if given) says why: EILSEQ, ENOMEM, or the OS's error from
// GetFullPathNameW. The I/O queue's paths come through here (os_file_open,
// os_path_kind, os_path_stat).
struct WidePath {
    wchar_t        small[KV_WIDE_PATH_MAX];
    wchar_t*       heap[2] = {nullptr, nullptr};
    const wchar_t* w = nullptr;

    WidePath() = default;
    WidePath(const WidePath&) = delete;
    WidePath& operator=(const WidePath&) = delete;
    ~WidePath() { for (wchar_t* p : heap) if (p) HeapFree(GetProcessHeap(), 0, p); }

    bool set(const char* s, i32* err = nullptr) {
        w = nullptr;
        int n = utf16_from_wtf8(s, nullptr, 0);
        if (n <= 0) { if (err) *err = EILSEQ; return false; }
        wchar_t* buf = n <= KV_WIDE_PATH_MAX ? small : grab(0, (usize)n);
        if (!buf) { if (err) *err = ENOMEM; return false; }
        utf16_from_wtf8(s, (u16*)buf, n);
        // CreateDirectoryW stops 12 short of MAX_PATH (room for an 8.3 name), so that is the line.
        if (n - 1 < MAX_PATH - 12 || (buf[0] == L'\\' && buf[1] == L'\\' && (buf[2] == L'?' || buf[2] == L'.') && buf[3] == L'\\')) { w = buf; return true; }
        for (wchar_t* p = buf; *p; ++p) if (*p == L'/') *p = L'\\';
        DWORD need = GetFullPathNameW(buf, 0, nullptr, nullptr);
        if (!need) { if (err) *err = errno_from_win32(GetLastError()); return false; }
        // Room for "\\?\UNC" ahead of the full path: 7 characters.
        wchar_t* full = grab(1, (usize)need + 8);
        if (!full) { if (err) *err = ENOMEM; return false; }
        DWORD got = GetFullPathNameW(buf, need, full + 7, nullptr);
        if (!got || got >= need) { if (err) *err = errno_from_win32(GetLastError()); return false; }
        if (full[7] == L'\\' && full[8] == L'\\') { // \\server\share: \\?\UNC\server\share
            memcpy(full + 1, L"\\\\?\\UNC", 7 * sizeof(wchar_t));
            w = full + 1;
        } else {                                  // C:\dir: \\?\C:\dir
            memcpy(full + 3, L"\\\\?\\", 4 * sizeof(wchar_t));
            w = full + 3;
        }
        return true;
    }

private:
    wchar_t* grab(int i, usize chars) { return heap[i] = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, chars * sizeof(wchar_t)); }
};

// Backslashes to the engine's '/'.
inline void forward_slashes(char* s) { for (; *s; ++s) if (*s == '\\') *s = '/'; }

} // namespace kv
