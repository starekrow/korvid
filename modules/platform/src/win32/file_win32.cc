// Files and directories on Windows: handles from CreateFileW, positional ReadFile, the
// FindFirstFileExW walk. Handles are not inheritable, as O_CLOEXEC makes descriptors.
#include "../os_file.h"
#include "korvid/core/format.h"
#include "wide.h"
#include <string.h>
#include <wchar.h>

namespace kv {

namespace {
i32 fail_errno() { return errno_from_win32(GetLastError()); }
HANDLE file_handle(OsFile f) { return (HANDLE)(intptr_t)f; }
} // namespace

OsFile os_file_open(const char* path, bool write, i32* err) {
    wchar_t w[KV_WIDE_PATH_MAX];
    if (!widen(path, w, KV_WIDE_PATH_MAX, err)) return -1;
    // Readers let others read, write and delete meanwhile, as POSIX descriptors do.
    HANDLE h = write ? CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)
                     : CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { *err = fail_errno(); return -1; }
    return (OsFile)(intptr_t)h;
}

void os_file_close(OsFile f) { if (f >= 0) CloseHandle(file_handle(f)); }

bool os_file_size(OsFile f, u64* size, i32* err) {
    LARGE_INTEGER li;
    if (!GetFileSizeEx(file_handle(f), &li)) { *err = fail_errno(); return false; }
    *size = (u64)li.QuadPart;
    return true;
}

// An offset in the OVERLAPPED of a synchronous handle reads there without seeking (the
// position moves, but nothing here uses it). One call moves at most 1 GiB.
i64 os_file_read_at(OsFile f, void* buf, usize n, u64 at, i32* err) {
    OVERLAPPED ov = {};
    ov.Offset     = (DWORD)at;
    ov.OffsetHigh = (DWORD)(at >> 32);
    DWORD want = n > (1u << 30) ? (1u << 30) : (DWORD)n;
    DWORD got = 0;
    if (ReadFile(file_handle(f), buf, want, &got, &ov)) return got;
    if (GetLastError() == ERROR_HANDLE_EOF) return 0;
    *err = fail_errno();
    return -1;
}

i64 os_file_write(OsFile f, const void* buf, usize n, i32* err) {
    DWORD want = n > (1u << 30) ? (1u << 30) : (DWORD)n;
    DWORD put = 0;
    if (WriteFile(file_handle(f), buf, want, &put, nullptr)) return put;
    *err = fail_errno();
    return -1;
}

OsPathKind os_path_kind(const char* path) {
    wchar_t w[KV_WIDE_PATH_MAX];
    if (!widen(path, w, KV_WIDE_PATH_MAX)) return OsPathKind::None;
    // Opening it follows links, as stat does; the attributes alone would describe the link.
    HANDLE h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return OsPathKind::None;
    BY_HANDLE_FILE_INFORMATION info;
    BOOL ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    if (!ok) return OsPathKind::None;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return OsPathKind::Dir;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) return OsPathKind::Other;
    return OsPathKind::File;
}

bool os_dir_each(const char* dir, void (*fn)(void* user, const char* name), void* user) {
    wchar_t w[KV_WIDE_PATH_MAX];
    if (!widen(dir, w, KV_WIDE_PATH_MAX - 2)) return false;
    usize n = wcslen(w);
    if (n && w[n - 1] != L'/' && w[n - 1] != L'\\') w[n++] = L'/';
    w[n++] = L'*';
    w[n] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(w, FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND; // empty: no entries at all
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        char name[MAX_PATH * 4];
        if (narrow(fd.cFileName, name, (int)sizeof name)) fn(user, name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

bool os_make_dir(const char* path) {
    if (os_path_kind(path) == OsPathKind::Dir) return true; // also a drive, "C:"
    wchar_t w[KV_WIDE_PATH_MAX];
    if (!widen(path, w, KV_WIDE_PATH_MAX)) return false;
    return CreateDirectoryW(w, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool os_remove(const char* path) {
    wchar_t w[KV_WIDE_PATH_MAX];
    if (!widen(path, w, KV_WIDE_PATH_MAX)) return false;
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES) return false;
    if (a & FILE_ATTRIBUTE_DIRECTORY) return RemoveDirectoryW(w) != 0;
    return DeleteFileW(w) != 0;
}

// Under the user's temporary directory (GetTempPath2W where Windows has it, which keeps a
// SYSTEM process's files private), named prefix-<pid>-<n>; CreateDirectoryW failing on a
// name that exists makes the choice race-free, as mkdtemp's is.
bool os_make_temp_dir(const char* prefix, char* out, u32 cap) {
    wchar_t tmp[MAX_PATH + 1];
    typedef DWORD(WINAPI * TempPath2)(DWORD, LPWSTR);
    static TempPath2 temp_path2 = (TempPath2)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTempPath2W");
    DWORD n = temp_path2 ? temp_path2(MAX_PATH + 1, tmp) : GetTempPathW(MAX_PATH + 1, tmp);
    if (n == 0 || n > MAX_PATH) return false;
    // The long form: GetTempPath can answer with 8.3 names (C:\Users\DAVIDO~1\...).
    wchar_t full[KV_WIDE_PATH_MAX];
    DWORD m = GetLongPathNameW(tmp, full, KV_WIDE_PATH_MAX);
    if (m == 0 || m >= (DWORD)KV_WIDE_PATH_MAX) wcscpy_s(full, tmp);
    char base[KV_WIDE_PATH_MAX * 3];
    if (!narrow(full, base, (int)sizeof base)) return false;
    forward_slashes(base);
    usize len = strlen(base);
    while (len > 1 && base[len - 1] == '/') base[--len] = 0;
    static u32 counter = 0;
    for (u32 attempt = 0; attempt < 1000; ++attempt) {
        int k = kv::format(out, cap, "%s/%s-%lu-%u", base, prefix, (unsigned long)GetCurrentProcessId(), ++counter);
        if (k <= 0 || (u32)k >= cap) return false;
        wchar_t w[KV_WIDE_PATH_MAX];
        if (!widen(out, w, KV_WIDE_PATH_MAX)) return false;
        if (CreateDirectoryW(w, nullptr)) return true;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    return false;
}

} // namespace kv
