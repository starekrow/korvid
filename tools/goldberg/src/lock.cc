// goldberg: the lock that keeps two goldbergs off one output tree. A file beside the
// state, locked with flock for as long as the process lives: the kernel lets go of it
// when the process ends, however it ends, so there is no stale lock to clear. The file
// holds a line saying who has it ("watch 1234"), for whoever finds it taken.
#include "korvid/core/format.h"
#include "korvid/core/types.h"
#if !defined(_WIN32)
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
#if defined(_WIN32)
    (void)path; (void)who; (void)cap;
    *handle = -1;
    return 1; // not kept apart on Windows yet
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
#if !defined(_WIN32)
    if (handle >= 0) close((int)handle);
#else
    (void)handle;
#endif
}

} // namespace kv
