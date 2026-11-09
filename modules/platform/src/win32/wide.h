// Korvid platform on Windows: the edge between the engine's UTF-8 and the wide (UTF-16)
// Win32 API, and Win32 errors as the errno values the rest of the engine speaks. Every
// Windows source in this module includes it; inline, so a unity build sees it once.
#pragma once
#include "korvid/core/types.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <errno.h>

namespace kv {

// A path's room in UTF-16: the engine's own path buffers are at most 1024 bytes.
constexpr int KV_WIDE_PATH_MAX = 1024;

// UTF-8 into out (cap characters, terminated). False, with *err = ENAMETOOLONG or EILSEQ,
// if it does not fit or is not UTF-8.
inline bool widen(const char* s, wchar_t* out, int cap, i32* err = nullptr) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, cap);
    if (n > 0) return true;
    if (err) *err = GetLastError() == ERROR_INSUFFICIENT_BUFFER ? ENAMETOOLONG : EILSEQ;
    return false;
}

// UTF-16 into out as UTF-8 (cap bytes, terminated). False if it does not fit.
inline bool narrow(const wchar_t* w, char* out, int cap) {
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, nullptr, nullptr) > 0;
}

// Backslashes to the engine's '/'.
inline void forward_slashes(char* s) { for (; *s; ++s) if (*s == '\\') *s = '/'; }

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

} // namespace kv
