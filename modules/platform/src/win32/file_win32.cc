// Files and directories on Windows: handles from CreateFileW, positional ReadFile, the
// FindFirstFileExW walk. Handles are not inheritable, as O_CLOEXEC makes descriptors.
//
// Every file handle is opened FILE_FLAG_OVERLAPPED. The I/O ring (io_ring_win32.cc) is only
// asynchronous on such a handle: on a synchronous one Windows performs the ring's reads one
// after another inside SubmitIoRing (64 MB cold: 55 ms in the submit at 1.1 GB/s, against
// 1 ms and 3.1 GB/s overlapped, measured here). The blocking calls below wait for their own
// operation on an event of their own, so they can share a handle with the ring (a pack
// mount is read both ways). An overlapped handle has no file position: a sequential write
// goes at the end of the file, which for a file just created is the same thing.
#include "../os_file.h"
#include "korvid/core/format.h"
#include "wide.h"
#include <string.h>
#include <wchar.h>

namespace kv {

namespace {
i32 fail_errno() { return errno_from_win32(GetLastError()); }
HANDLE file_handle(KHandle f) { return (HANDLE)(intptr_t)f.os(); }
KHandle file_of(HANDLE h) { return khandle_make(KHandleKind::File, (i64)(intptr_t)h); }

// One operation on an overlapped handle, waited for: the bytes moved, 0 at the end of the
// file, or -1 with *err set. The event is the operation's own, so a completion of another
// operation on the same handle (the ring's) cannot wake it.
i64 overlapped_wait(HANDLE h, bool write, void* buf, DWORD want, u64 at, i32* err) {
    OVERLAPPED ov = {};
    ov.Offset     = (DWORD)at;
    ov.OffsetHigh = (DWORD)(at >> 32);
    ov.hEvent     = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) { *err = fail_errno(); return -1; }
    BOOL started = write ? WriteFile(h, buf, want, nullptr, &ov) : ReadFile(h, buf, want, nullptr, &ov);
    DWORD moved = 0, e = 0;
    if (!started && GetLastError() != ERROR_IO_PENDING) e = GetLastError();
    else if (!GetOverlappedResult(h, &ov, &moved, TRUE)) e = GetLastError();
    CloseHandle(ov.hEvent);
    if (!e) return moved;
    if (!write && e == ERROR_HANDLE_EOF) return 0;
    *err = errno_from_win32(e);
    return -1;
}
} // namespace

KHandle os_file_open(const char* path, bool write, i32* err) {
    WidePath wp;
    if (!wp.set(path, err)) return KHandle{};
    const wchar_t* w = wp.w;
    // Readers let others read, write and delete meanwhile, as POSIX descriptors do.
    // Overlapped, for the ring: see the top of the file.
    HANDLE h = write ? CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr)
                     : CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) { *err = fail_errno(); return KHandle{}; }
    return file_of(h);
}

void os_file_close(KHandle f) { if (f.is_open()) CloseHandle(file_handle(f)); }

bool os_file_sync(KHandle f, i32* err) {
    if (FlushFileBuffers(file_handle(f))) return true;
    *err = fail_errno();
    return false;
}

KHandle os_file_lock(const char* path, i32* err) {
    WidePath wp;
    if (!wp.set(path, err)) return KHandle{};
    HANDLE h = CreateFileW(wp.w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { *err = fail_errno(); return KHandle{}; }
    OVERLAPPED ov{};
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &ov)) { *err = fail_errno(); CloseHandle(h); return KHandle{}; }
    return file_of(h); // closing it lets the lock go
}

void os_dir_sync(const char*) {}

bool os_file_size(KHandle f, u64* size, i32* err) {
    LARGE_INTEGER li;
    if (!GetFileSizeEx(file_handle(f), &li)) { *err = fail_errno(); return false; }
    *size = (u64)li.QuadPart;
    return true;
}

// At the offset, waited for. One call moves at most 1 GiB.
i64 os_file_read_at(KHandle f, void* buf, usize n, u64 at, i32* err) {
    DWORD want = n > (1u << 30) ? (1u << 30) : (DWORD)n;
    return overlapped_wait(file_handle(f), false, buf, want, at, err);
}

// At the end of the file (an offset of all ones: an overlapped handle has no position), so
// a file written from its creation is written in order. One call moves at most 1 GiB.
i64 os_file_write(KHandle f, const void* buf, usize n, i32* err) {
    DWORD want = n > (1u << 30) ? (1u << 30) : (DWORD)n;
    return overlapped_wait(file_handle(f), true, (void*)buf, want, ~0ull, err);
}

// The entry's own record, as a listing of its directory has it: the reparse tag says a
// link (or a junction) from the other kinds of reparse point (a cloud placeholder, a
// deduplicated file), which are what they look like.
bool os_path_is_dir_link(const char* path) {
    WidePath wp;
    if (!wp.set(path)) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(wp.w, FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
           (fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK || fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT) &&
           os_path_kind(path) == OsPathKind::Dir;
}

OsPathKind os_path_kind(const char* path) {
    WidePath wp;
    if (!wp.set(path)) return OsPathKind::None;
    const wchar_t* w = wp.w;
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

namespace {
// The serial number of the volume a path (a WidePath's) is on, as GetFileInformationByHandle
// would give it, with no handle on the file: the path's volume (GetVolumePathNameW, which
// sees a volume mounted on a folder), then that volume's information. 0 if either fails.
u64 volume_serial(const wchar_t* w) {
    usize cap = wcslen(w) + 2; // the volume's path is never longer than the path and a trailing backslash
    wchar_t small[MAX_PATH + 2];
    wchar_t* root = cap <= MAX_PATH + 2 ? small : (wchar_t*)HeapAlloc(GetProcessHeap(), 0, cap * sizeof(wchar_t));
    if (!root) return 0;
    DWORD serial = 0;
    bool ok = GetVolumePathNameW(w, root, (DWORD)cap) &&
              GetVolumeInformationW(root, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0);
    if (root != small) HeapFree(GetProcessHeap(), 0, root);
    return ok ? serial : 0;
}

// A directory's first entry, the directory a WidePath's (so of any length): FindFirstFileExW
// on its \* pattern. INVALID_HANDLE_VALUE, with *first the reason, if it gives none.
HANDLE find_first(const wchar_t* dir, WIN32_FIND_DATAW* fd, DWORD* first) {
    usize n = wcslen(dir);
    wchar_t* pattern = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, (n + 3) * sizeof(wchar_t));
    if (!pattern) { *first = ERROR_NOT_ENOUGH_MEMORY; return INVALID_HANDLE_VALUE; }
    memcpy(pattern, dir, n * sizeof(wchar_t));
    // The \\?\ form takes no '/': the separator is a backslash either way.
    if (n && pattern[n - 1] != L'/' && pattern[n - 1] != L'\\') pattern[n++] = L'\\';
    pattern[n++] = L'*';
    pattern[n] = 0;
    HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    *first = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    HeapFree(GetProcessHeap(), 0, pattern);
    return h;
}
} // namespace

bool os_path_stat(const char* path, OsStat* out, i32* err) {
    *out = OsStat{};
    WidePath wp;
    if (!wp.set(path, err)) return false;
    const wchar_t* w = wp.w;
    // The attributes, the size and the time come with the directory's own record: no handle
    // is opened, which on Windows is the cost (every filter driver, the antivirus's among
    // them, sees an open). Not for a reparse point (a symbolic link, a junction), which
    // that record describes as itself: that is opened below, and followed, as before.
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &fa)) { if (err) *err = fail_errno(); return false; }
    if (!(fa.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        out->kind = (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? OsPathKind::Dir
                  : (fa.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) ? OsPathKind::Other : OsPathKind::File;
        out->size = ((u64)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        out->mtime_ns = (i64)((((u64)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime) * 100ull);
        out->volume = volume_serial(w);
        return true;
    }
    HANDLE h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) { if (err) *err = fail_errno(); return false; }
    BY_HANDLE_FILE_INFORMATION info;
    BOOL ok = GetFileInformationByHandle(h, &info);
    if (!ok && err) *err = fail_errno();
    CloseHandle(h);
    if (!ok) return false;
    out->kind = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? OsPathKind::Dir
              : (info.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) ? OsPathKind::Other : OsPathKind::File;
    out->size = ((u64)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    // FILETIME: 100 ns units. Only ever compared with another.
    out->mtime_ns = (i64)((((u64)info.ftLastWriteTime.dwHighDateTime << 32) | info.ftLastWriteTime.dwLowDateTime) * 100ull);
    out->volume = info.dwVolumeSerialNumber; // the link's target's
    return true;
}

// A share by its name (\\server\share, \\?\UNC\...), or a drive Windows calls remote: a
// mapped network drive, WebDAV. The volume's path first (GetVolumePathNameW), so a share
// mounted on a local folder is found too.
bool os_path_is_remote(const char* path) {
    WidePath wp;
    if (!wp.set(path)) return false;
    usize cap = wcslen(wp.w) + 2;
    wchar_t small[MAX_PATH + 2];
    wchar_t* root = cap <= MAX_PATH + 2 ? small : (wchar_t*)HeapAlloc(GetProcessHeap(), 0, cap * sizeof(wchar_t));
    if (!root) return false;
    bool remote = false;
    if (GetVolumePathNameW(wp.w, root, (DWORD)cap)) {
        bool unc = (root[0] == L'\\' && root[1] == L'\\' && root[2] != L'?' && root[2] != L'.') ||
                   wcsncmp(root, L"\\\\?\\UNC\\", 8) == 0;
        remote = unc || GetDriveTypeW(root) == DRIVE_REMOTE;
    }
    if (root != small) HeapFree(GetProcessHeap(), 0, root);
    return remote;
}

// A replace can be refused for a moment when something outside the process has just opened
// the file without delete sharing (an antivirus scanning what was written, the indexer):
// access denied or a sharing violation (seen: the settings store's two writers, about one
// write in a thousand, through on the first retry). Asked again every 5 ms for half a second
// before it is a failure; one that is really refused (a directory, a read-only file) costs
// that half second. errno says why, as rename's does. Paths of any length (WidePath).
bool os_rename(const char* from, const char* to, i32* err) {
    WidePath wf, wt;
    i32 why = 0;
    if (!wf.set(from, &why) || !wt.set(to, &why)) { errno = why; return os_fail(err, why); }
    for (u32 attempt = 0;; ++attempt) {
        if (MoveFileExW(wf.w, wt.w, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        DWORD e = GetLastError();
        bool held = e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION;
        if (!held || attempt == 100) { errno = errno_from_win32(e); return os_fail(err, errno); }
        Sleep(5);
    }
}

bool os_current_dir(char* out, u32 cap, i32* err) {
    wchar_t w[KV_WIDE_PATH_MAX];
    DWORD n = GetCurrentDirectoryW(KV_WIDE_PATH_MAX, w);
    if (n == 0) return os_fail(err, fail_errno());
    if (n >= (DWORD)KV_WIDE_PATH_MAX || !narrow(w, out, (int)cap)) return os_fail(err, ENAMETOOLONG);
    forward_slashes(out);
    return true;
}

bool os_dir_each(const char* dir, void (*fn)(void* user, const char* name), void* user, i32* err) {
    WidePath wp;
    if (!wp.set(dir, err)) return false;
    WIN32_FIND_DATAW fd;
    DWORD first;
    HANDLE h = find_first(wp.w, &fd, &first);
    if (h == INVALID_HANDLE_VALUE) return first == ERROR_FILE_NOT_FOUND || os_fail(err, errno_from_win32(first)); // empty: no entries at all
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        char name[MAX_PATH * 4];
        if (narrow(fd.cFileName, name, (int)sizeof name)) fn(user, name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

// The listing's own record of each entry: its attributes, size and last write, as
// os_path_stat reads them, with no query a file. A reparse point (a symbolic link, a
// junction) is the one asked, by its path, so that it is followed as os_path_stat follows it.
bool os_dir_each_stat(const char* dir, void (*fn)(void* user, const char* name, const OsStat& st), void* user, i32* err) {
    WidePath wp;
    if (!wp.set(dir, err)) return false;
    WIN32_FIND_DATAW fd;
    DWORD first;
    HANDLE h = find_first(wp.w, &fd, &first);
    if (h == INVALID_HANDLE_VALUE) return first == ERROR_FILE_NOT_FOUND || os_fail(err, errno_from_win32(first)); // empty: no entries at all
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        char name[MAX_PATH * 4];
        if (!narrow(fd.cFileName, name, (int)sizeof name)) continue;
        OsStat st;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            char path[KV_WIDE_PATH_MAX * 3];
            if (format(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path || !os_path_stat(path, &st)) continue;
        } else {
            st.kind = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? OsPathKind::Dir
                    : (fd.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) ? OsPathKind::Other : OsPathKind::File;
            st.size = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            st.mtime_ns = (i64)((((u64)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime) * 100ull);
        }
        fn(user, name, st);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

// FindFirstFileExW's records carry every entry's attributes, size and last write, so
// `sizes` costs nothing here and the listing asks nothing of a file. A symbolic link or a
// junction (a reparse point with either tag) is a link, asked by its path so that its
// target is described; other reparse points (OneDrive's placeholders, deduplicated files)
// are what their record says. The directory goes through WidePath, so it may be long.
bool os_dir_list(const char* dir, bool sizes, void (*fn)(void* user, const OsDirEntry& e), void* user, i32* err) {
    WidePath wp;
    if (!wp.set(dir, err)) return false;
    WIN32_FIND_DATAW fd;
    DWORD first;
    HANDLE h = find_first(wp.w, &fd, &first);
    if (h == INVALID_HANDLE_VALUE) {
        if (first == ERROR_FILE_NOT_FOUND) return true; // a drive's root with nothing on it: not even . and ..
        // A file named as the directory: "not a directory", as POSIX says it.
        if (first == ERROR_DIRECTORY || (first == ERROR_PATH_NOT_FOUND && os_path_kind(dir) == OsPathKind::File)) first = ERROR_DIRECTORY;
        if (err) *err = errno_from_win32(first);
        return false;
    }
    // Every entry that is not a link is on the directory's volume: asked once.
    u64 volume = sizes ? volume_serial(wp.w) : 0;
    DWORD failed = 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        char name[MAX_PATH * 4];
        if (!narrow(fd.cFileName, name, (int)sizeof name)) continue;
        OsDirEntry e;
        e.name   = name;
        e.hidden = name[0] == '.' || (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN);
        bool reparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        e.is_link = reparse && (fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK || fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT);
        if (e.is_link) {
            Array<char> path;
            usize dl = strlen(dir), nl = strlen(name);
            char* p = path.push_n((u32)(dl + nl + 2));
            memcpy(p, dir, dl);
            p[dl] = '/';
            memcpy(p + dl + 1, name, nl + 1);
            if (!os_path_stat(p, &e.st)) e.st = OsStat{}; // nothing at the far end: None
            path.free();
        } else {
            e.st.kind = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? OsPathKind::Dir
                      : (fd.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) ? OsPathKind::Other : OsPathKind::File;
            e.st.size = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            e.st.mtime_ns = (i64)((((u64)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime) * 100ull);
            e.st.volume = volume;
        }
        if (!sizes) { e.st.size = 0; e.st.mtime_ns = 0; e.st.volume = 0; }
        fn(user, e);
    } while (FindNextFileW(h, &fd));
    if (GetLastError() != ERROR_NO_MORE_FILES) failed = GetLastError();
    FindClose(h);
    if (failed) { if (err) *err = errno_from_win32(failed); return false; }
    return true;
}

bool os_make_dir(const char* path, i32* err) {
    if (os_path_kind(path) == OsPathKind::Dir) return true; // also a drive, "C:"
    WidePath wp;
    if (!wp.set(path, err)) return false;
    if (CreateDirectoryW(wp.w, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) return true;
    return os_fail(err, fail_errno());
}

bool os_remove(const char* path, i32* err) {
    WidePath wp;
    if (!wp.set(path, err)) return false;
    DWORD a = GetFileAttributesW(wp.w);
    if (a == INVALID_FILE_ATTRIBUTES) return os_fail(err, fail_errno());
    if ((a & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(wp.w) : DeleteFileW(wp.w)) return true;
    return os_fail(err, fail_errno());
}

bool os_remove_dir(const char* path, i32* err) {
    WidePath wp;
    if (!wp.set(path, err)) return false;
    if (RemoveDirectoryW(wp.w)) return true;
    DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true;
    return os_fail(err, errno_from_win32(e));
}

// Under the user's temporary directory (GetTempPath2W where Windows has it, which keeps a
// SYSTEM process's files private), named prefix-<pid>-<n>; CreateDirectoryW failing on a
// name that exists makes the choice race-free, as mkdtemp's is.
bool os_make_temp_dir(const char* prefix, char* out, u32 cap, i32* err) {
    wchar_t tmp[MAX_PATH + 1];
    typedef DWORD(WINAPI * TempPath2)(DWORD, LPWSTR);
    static TempPath2 temp_path2 = (TempPath2)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTempPath2W");
    DWORD n = temp_path2 ? temp_path2(MAX_PATH + 1, tmp) : GetTempPathW(MAX_PATH + 1, tmp);
    if (n == 0 || n > MAX_PATH) return os_fail(err, n ? ENAMETOOLONG : fail_errno());
    // The long form: GetTempPath can answer with 8.3 names (C:\Users\DAVIDO~1\...).
    wchar_t full[KV_WIDE_PATH_MAX];
    DWORD m = GetLongPathNameW(tmp, full, KV_WIDE_PATH_MAX);
    if (m == 0 || m >= (DWORD)KV_WIDE_PATH_MAX) wcscpy_s(full, tmp);
    char base[KV_WIDE_PATH_MAX * 3];
    if (!narrow(full, base, (int)sizeof base)) return os_fail(err, ENAMETOOLONG);
    forward_slashes(base);
    usize len = strlen(base);
    while (len > 1 && base[len - 1] == '/') base[--len] = 0;
    static u32 counter = 0;
    for (u32 attempt = 0; attempt < 1000; ++attempt) {
        int k = kv::format(out, cap, "%s/%s-%lu-%u", base, prefix, (unsigned long)GetCurrentProcessId(), ++counter);
        if (k <= 0 || (u32)k >= cap) return os_fail(err, ENAMETOOLONG);
        wchar_t w[KV_WIDE_PATH_MAX];
        if (!widen(out, w, KV_WIDE_PATH_MAX, err)) return false;
        if (CreateDirectoryW(w, nullptr)) return true;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return os_fail(err, fail_errno());
    }
    return os_fail(err, EEXIST);
}

} // namespace kv

namespace kv {

// LockFileEx on one byte far past the end: a Windows lock also keeps others from reading
// what it covers, and the holder's line must stay readable. Not overlapped (the lock is
// asked for and answered at once), not inherited, so a child the holder starts does not
// hold it.
KHandle os_file_lock_try(const char* path, const char* who, char* holder, u32 cap, i32* err) {
    if (holder && cap) holder[0] = 0;
    WidePath wp;
    if (!wp.set(path, err)) return KHandle{};
    HANDLE h = CreateFileW(wp.w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { if (err) *err = fail_errno(); return KHandle{}; }
    OVERLAPPED at{};
    at.Offset     = 0xFFFFFFFEu; // the lock's byte: past any line, so the line stays readable
    at.OffsetHigh = 0x7FFFFFFFu;
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at)) {
        DWORD why = GetLastError();
        bool held = why == ERROR_LOCK_VIOLATION || why == ERROR_IO_PENDING || why == ERROR_SHARING_VIOLATION;
        if (held && holder && cap) {
            DWORD n = 0;
            if (!ReadFile(h, holder, (DWORD)(cap - 1), &n, nullptr)) n = 0;
            holder[n] = 0;
            if (char* nl = strchr(holder, '\n')) *nl = 0;
            if (char* cr = strchr(holder, '\r')) *cr = 0;
        }
        CloseHandle(h);
        if (err) *err = held ? EWOULDBLOCK : errno_from_win32(why);
        return KHandle{};
    }
    char line[160];
    int n = kv::format(line, sizeof line, "%s %lu\n", who ? who : "", (unsigned long)GetCurrentProcessId());
    DWORD wrote = 0;
    if (n > 0 && SetFilePointer(h, 0, nullptr, FILE_BEGIN) == 0 && SetEndOfFile(h)) WriteFile(h, line, (DWORD)n, &wrote, nullptr);
    return file_of(h); // closing it lets the lock go
}

} // namespace kv
