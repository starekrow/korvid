// goldberg: the lock that keeps two goldbergs off one output tree. A file beside the
// state, locked for as long as the process lives: the OS lets go of it when the process
// ends, however it ends, so there is no stale lock to clear. The file holds a line saying
// who has it ("watch 1234"), for whoever finds it taken.
//
// posix: flock on the whole file. Windows: LockFileEx on one byte far past the end, since
// a Windows lock also keeps others from reading what it covers and the line must stay
// readable; the handle is not inherited, so a converter goldberg starts does not hold it.
#include "korvid/core/format.h"
#include "korvid/core/types.h"
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef near
#undef far
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#include <string.h>

namespace kv {

// Takes the lock at `path`, writing `who` into it. 1: taken (and held until the process
// ends or goldberg_unlock). 0: someone else has it; `holder` gets their line. -1: the
// file cannot be made.
int goldberg_lock(const char* path, const char* who, char* holder, usize cap, i64* handle) {
    holder[0] = 0;
    *handle = -1;
#if defined(_WIN32)
    wchar_t wpath[1024];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, 1024) <= 0) return -1;
    HANDLE h = CreateFileW(wpath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED at{};
    at.Offset     = 0xFFFFFFFEu; // the lock's byte: past any line, so the line stays readable
    at.OffsetHigh = 0x7FFFFFFFu;
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at)) {
        DWORD n = 0;
        if (!ReadFile(h, holder, (DWORD)(cap - 1), &n, nullptr)) n = 0;
        holder[n] = 0;
        if (char* nl = strchr(holder, '\n')) *nl = 0;
        if (char* cr = strchr(holder, '\r')) *cr = 0;
        CloseHandle(h);
        return 0;
    }
    char line[128];
    int n = format(line, sizeof line, "%s %lu\n", who, (unsigned long)GetCurrentProcessId());
    DWORD wrote = 0;
    if (SetFilePointer(h, 0, nullptr, FILE_BEGIN) == 0 && SetEndOfFile(h)) WriteFile(h, line, (DWORD)n, &wrote, nullptr);
    *handle = (i64)(intptr_t)h;
    return 1;
#else
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ssize_t n = read(fd, holder, cap - 1);
        holder[n > 0 ? n : 0] = 0;
        if (char* nl = strchr(holder, '\n')) *nl = 0;
        close(fd);
        return 0;
    }
    char line[128];
    int n = format(line, sizeof line, "%s %d\n", who, (int)getpid());
    if (ftruncate(fd, 0) == 0 && write(fd, line, (usize)n) < 0) {}
    *handle = fd;
    return 1;
#endif
}

void goldberg_unlock(i64 handle) {
    if (handle < 0) return;
#if defined(_WIN32)
    CloseHandle((HANDLE)(intptr_t)handle); // closing it lets go of the lock
#else
    close((int)handle);
#endif
}

} // namespace kv
