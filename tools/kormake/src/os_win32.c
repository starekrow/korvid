/* kormake: the operating system on Windows. See os.h. Every call goes through the wide
 * (UTF-16) Win32 API: strings cross this file as UTF-8 and are converted at the edge, and
 * paths handed back use '/' so the scripts see the same form as on POSIX hosts. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "os.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static DWORD g_error;
static char  g_error_text[512];

static int fail(void) { g_error = GetLastError(); return -1; }

const char* kos_error(void) {
    wchar_t w[512];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, g_error,
                             0, w, (DWORD)(sizeof w / sizeof w[0]), NULL);
    while (n > 0 && (w[n - 1] == L'\r' || w[n - 1] == L'\n' || w[n - 1] == L'.')) w[--n] = 0;
    if (n == 0 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, g_error_text, (int)sizeof g_error_text, NULL, NULL))
        snprintf(g_error_text, sizeof g_error_text, "error %lu", (unsigned long)g_error);
    return g_error_text;
}

/* UTF-8 to a fresh UTF-16 string, freed by the caller; NULL (with the error set) if the
 * input is not valid UTF-8. */
static wchar_t* widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) { fail(); return NULL; }
    wchar_t* w = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n);
    return w;
}

/* UTF-16 into buf as UTF-8, separators turned to '/'. */
static int narrow_path(const wchar_t* w, char* buf, size_t n) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)n, NULL, NULL)) return fail();
    for (char* p = buf; *p; ++p) if (*p == '\\') *p = '/';
    return 0;
}

/* The wide entry point: argv arrives as UTF-16 and leaves as UTF-8. The console shows
 * UTF-8 output for the length of the run and is put back on the way out. */
static UINT g_console_cp;
static void restore_console(void) { if (g_console_cp) SetConsoleOutputCP(g_console_cp); }

int wmain(int argc, wchar_t** wargv) {
    g_console_cp = GetConsoleOutputCP();
    if (g_console_cp && g_console_cp != CP_UTF8) { SetConsoleOutputCP(CP_UTF8); atexit(restore_console); }
    char** argv = (char**)calloc((size_t)argc + 1, sizeof(char*));
    for (int i = 0; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = (char*)malloc((size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], n, NULL, NULL);
    }
    return kormake_main(argc, argv);
}

FILE* kos_fopen(const char* path, const char* mode) {
    wchar_t* wp = widen(path);
    wchar_t* wm = widen(mode);
    FILE* f = NULL;
    if (wp && wm) {
        f = _wfopen(wp, wm);
        if (!f) g_error = _doserrno ? (DWORD)_doserrno : ERROR_FILE_NOT_FOUND;
    }
    free(wp);
    free(wm);
    return f;
}

/* FILETIME (100 ns ticks since 1601) to seconds since 1970. */
static long long unix_seconds(FILETIME ft) {
    ULARGE_INTEGER t;
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    return (long long)((t.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

int kos_stat(const char* path, KosStat* out) {
    wchar_t* w = widen(path);
    if (!w) return -1;
    /* Opening the file follows links, as stat does; the attributes alone would describe
     * the link. Backup semantics lets a directory be opened. */
    HANDLE h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return fail();
    BY_HANDLE_FILE_INFORMATION info;
    BOOL ok = GetFileInformationByHandle(h, &info);
    if (!ok) fail();
    CloseHandle(h);
    if (!ok) return -1;
    out->is_dir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out->is_file = !out->is_dir && !(info.dwFileAttributes & FILE_ATTRIBUTE_DEVICE);
    out->mtime = unix_seconds(info.ftLastWriteTime);
    return 0;
}

/* dir + "\\" + name as a fresh wide string. */
static wchar_t* wjoin(const wchar_t* dir, const wchar_t* name) {
    size_t a = wcslen(dir), b = wcslen(name);
    wchar_t* w = (wchar_t*)malloc((a + b + 2) * sizeof(wchar_t));
    memcpy(w, dir, a * sizeof(wchar_t));
    size_t at = a;
    if (a == 0 || (dir[a - 1] != L'\\' && dir[a - 1] != L'/')) w[at++] = L'\\';
    memcpy(w + at, name, (b + 1) * sizeof(wchar_t));
    return w;
}

int kos_listdir(const char* path, void (*fn)(void* user, const char* name), void* user) {
    wchar_t* w = widen(path);
    if (!w) return -1;
    wchar_t* pattern = wjoin(w, L"*");
    free(w);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return fail();
    do {
        char name[MAX_PATH * 4];
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, (int)sizeof name, NULL, NULL)) fn(user, name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}

int kos_mkdir(const char* path) {
    KosStat st;
    if (kos_stat(path, &st) == 0 && st.is_dir) return 0;   /* also covers a drive, "C:" */
    wchar_t* w = widen(path);
    if (!w) return -1;
    BOOL ok = CreateDirectoryW(w, NULL);
    if (!ok) fail();
    free(w);
    return ok || g_error == ERROR_ALREADY_EXISTS ? 0 : -1;
}

/* A link or junction to a directory is removed itself, never followed; read-only files
 * and directories are made writable first, as rm -rf would remove them. */
static int rmtree_w(const wchar_t* path) {
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path, a & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(path) ? 0 : fail();
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        wchar_t* pattern = wjoin(path, L"*");
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
        free(pattern);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                wchar_t* child = wjoin(path, fd.cFileName);
                rmtree_w(child);
                free(child);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    return RemoveDirectoryW(path) ? 0 : fail();
}

int kos_rmtree(const char* path) {
    wchar_t* w = widen(path);
    if (!w) return -1;
    int rc = rmtree_w(w);
    free(w);
    return rc;
}

int kos_touch(const char* path) {
    wchar_t* w = widen(path);
    if (!w) return -1;
    HANDLE h = CreateFileW(w, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return fail();
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    BOOL ok = SetFileTime(h, NULL, NULL, &now);
    if (!ok) fail();
    CloseHandle(h);
    return ok ? 0 : -1;
}

int kos_copy(const char* from, const char* to) {
    wchar_t* wf = widen(from);
    wchar_t* wt = widen(to);
    BOOL ok = wf && wt && CopyFileW(wf, wt, FALSE);
    if (!ok && wf && wt) fail();
    free(wf);
    free(wt);
    return ok ? 0 : -1;
}

int kos_cwd(char* buf, size_t n) {
    DWORD len = GetCurrentDirectoryW(0, NULL);
    if (len == 0) return fail();
    wchar_t* w = (wchar_t*)malloc(len * sizeof(wchar_t));
    int rc = GetCurrentDirectoryW(len, w) ? narrow_path(w, buf, n) : fail();
    free(w);
    return rc;
}

/* Like realpath: the path must exist, and links are resolved, by asking the file system
 * for the final path of an open handle. The result loses the \\?\ prefix ("\\?\UNC\" for
 * a share becomes "//"). */
int kos_realpath(const char* path, char* buf, size_t n) {
    wchar_t* w = widen(path);
    if (!w) return -1;
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return fail();
    wchar_t final[32768];
    DWORD len = GetFinalPathNameByHandleW(h, final, (DWORD)(sizeof final / sizeof final[0]), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (len == 0 || len >= sizeof final / sizeof final[0]) { fail(); CloseHandle(h); return -1; }
    CloseHandle(h);
    const wchar_t* p = final;
    if (wcsncmp(p, L"\\\\?\\UNC\\", 8) == 0) {
        p += 6;                  /* keep two characters to become the leading "//" */
        final[6] = L'\\';
        final[7] = L'\\';
    } else if (wcsncmp(p, L"\\\\?\\", 4) == 0) {
        p += 4;
    }
    return narrow_path(p, buf, n);
}

int kos_self(char* buf, size_t n) {
    wchar_t w[32768];
    DWORD len = GetModuleFileNameW(NULL, w, (DWORD)(sizeof w / sizeof w[0]));
    if (len == 0 || len >= sizeof w / sizeof w[0]) return fail();
    return narrow_path(w, buf, n);
}

int kos_getenv(const char* name, char* buf, size_t n) {
    wchar_t* wn = widen(name);
    if (!wn) return 0;
    DWORD len = GetEnvironmentVariableW(wn, NULL, 0);
    int found = 0;
    if (len > 0) {
        wchar_t* v = (wchar_t*)malloc(len * sizeof(wchar_t));
        if (GetEnvironmentVariableW(wn, v, len) < len)
            found = WideCharToMultiByte(CP_UTF8, 0, v, -1, buf, (int)n, NULL, NULL) != 0;
        free(v);
    }
    free(wn);
    return found;
}

/* The command runs under cmd.exe, as system() would run it: /d skips AutoRun, and with
 * /s the outer pair of quotes added here is the only thing cmd strips, so a command that
 * itself starts with a quoted path survives intact. */
int kos_exec(const char* cmd) {
    fflush(stdout);
    fflush(stderr);
    wchar_t* wc = widen(cmd);
    if (!wc) return -1;
    wchar_t comspec[MAX_PATH];
    DWORD cl = GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
    if (cl == 0 || cl >= MAX_PATH) wcscpy(comspec, L"C:\\Windows\\System32\\cmd.exe");
    size_t len = wcslen(wc) + 32;
    wchar_t* line = (wchar_t*)malloc(len * sizeof(wchar_t));
    swprintf(line, len, L"cmd.exe /d /s /c \"%ls\"", wc);
    free(wc);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    BOOL ok = CreateProcessW(comspec, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    free(line);
    if (!ok) { fail(); return -1; }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
