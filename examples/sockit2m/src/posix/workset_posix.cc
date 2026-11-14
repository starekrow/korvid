// sockit2m's working set on POSIX: mkdtemp, sync, and the page cache dropped with
// posix_fadvise (Linux) or only flushed (macOS, which cannot drop one file's pages).
#include "../torture.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

namespace kv {

bool workset_make_unique_dir(char* path, i32* err) {
    if (mkdtemp(path)) return true;
    *err = errno;
    return false;
}

void workset_flush() { sync(); }

// Drops the working set from the page cache (clean pages after sync; no root needed), so
// the next read pass meets the disk rather than memory. macOS has no posix_fadvise and no
// unprivileged way to drop one file's pages (purge(8) is root's), so there the files are
// flushed and the eviction reports failure: the run then says the cold pass may be warm.
bool evict_working_set() {
#if defined(__APPLE__)
    for (u32 i = 0; i < g_opt.files; ++i) {
        char path[600];
        file_path(path, sizeof path, i);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) { fsync(fd); close(fd); }
    }
    return false;
#else
    bool ok = true;
    for (u32 i = 0; i < g_opt.files; ++i) {
        char path[600];
        file_path(path, sizeof path, i);
        int fd = open(path, O_RDONLY);
        if (fd < 0) { ok = false; continue; }
        fdatasync(fd);
        if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) != 0) ok = false;
        close(fd);
    }
    return ok;
#endif
}

void remove_working_set() {
    for (u32 i = 0; i < g_opt.files; ++i) { char path[600]; file_path(path, sizeof path, i); unlink(path); }
    rmdir(g_work_dir);
}

} // namespace kv
