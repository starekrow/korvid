// Files and directories on POSIX: descriptors, pread, opendir.
#include "../os_file.h"
#include "korvid/core/format.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kv {

OsFile os_file_open(const char* path, bool write, i32* err) {
    int fd = write ? open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644) : open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { *err = errno; return -1; }
    return fd;
}

void os_file_close(OsFile f) { if (f >= 0) close((int)f); }

bool os_file_size(OsFile f, u64* size, i32* err) {
    struct stat st;
    if (fstat((int)f, &st) != 0) { *err = errno; return false; }
    *size = (u64)st.st_size;
    return true;
}

i64 os_file_read_at(OsFile f, void* buf, usize n, u64 at, i32* err) {
    for (;;) {
        ssize_t r = pread((int)f, buf, n, (off_t)at);
        if (r >= 0) return r;
        if (errno == EINTR) continue;
        *err = errno;
        return -1;
    }
}

i64 os_file_write(OsFile f, const void* buf, usize n, i32* err) {
    for (;;) {
        ssize_t r = ::write((int)f, buf, n);
        if (r >= 0) return r;
        if (errno == EINTR) continue;
        *err = errno;
        return -1;
    }
}

OsPathKind os_path_kind(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return OsPathKind::None;
    if (S_ISREG(st.st_mode)) return OsPathKind::File;
    if (S_ISDIR(st.st_mode)) return OsPathKind::Dir;
    return OsPathKind::Other;
}

bool os_path_stat(const char* path, OsStat* out) {
    struct stat st;
    *out = OsStat{};
    if (stat(path, &st) != 0) return false;
    out->kind = S_ISREG(st.st_mode) ? OsPathKind::File : S_ISDIR(st.st_mode) ? OsPathKind::Dir : OsPathKind::Other;
    out->size = (u64)st.st_size;
#if defined(__APPLE__)
    out->mtime_ns = (i64)st.st_mtimespec.tv_sec * 1000000000ll + st.st_mtimespec.tv_nsec;
#else
    out->mtime_ns = (i64)st.st_mtim.tv_sec * 1000000000ll + st.st_mtim.tv_nsec;
#endif
    return true;
}

bool os_dir_each(const char* dir, void (*fn)(void* user, const char* name), void* user) {
    DIR* d = opendir(dir);
    if (!d) return false;
    while (dirent* de = readdir(d)) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        fn(user, de->d_name);
    }
    closedir(d);
    return true;
}

bool os_make_dir(const char* path) {
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

bool os_remove(const char* path) {
    return remove(path) == 0;   // ISO C: a file, or an empty directory on POSIX
}

bool os_make_temp_dir(const char* prefix, char* out, u32 cap) {
    const char* tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "/tmp";
    usize n = strlen(tmp);
    while (n > 1 && tmp[n - 1] == '/') --n;
    int len = format(out, cap, "%.*s/%s-XXXXXX", (int)n, tmp, prefix);
    if (len <= 0 || (u32)len >= cap) return false;
    return mkdtemp(out) != nullptr;
}

} // namespace kv
