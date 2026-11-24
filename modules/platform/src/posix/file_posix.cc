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
#if defined(__APPLE__)
#include <sys/mount.h>  // statfs, MNT_LOCAL
#else
#include <sys/vfs.h>    // statfs
#endif

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

bool os_path_is_dir_link(const char* path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode) && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void stat_to(const struct stat& st, OsStat* out) {
    out->kind = S_ISREG(st.st_mode) ? OsPathKind::File : S_ISDIR(st.st_mode) ? OsPathKind::Dir : OsPathKind::Other;
    out->size = (u64)st.st_size;
    out->volume = (u64)st.st_dev;
#if defined(__APPLE__)
    out->mtime_ns = (i64)st.st_mtimespec.tv_sec * 1000000000ll + st.st_mtimespec.tv_nsec;
#else
    out->mtime_ns = (i64)st.st_mtim.tv_sec * 1000000000ll + st.st_mtim.tv_nsec;
#endif
}

bool os_path_stat(const char* path, OsStat* out, i32* err) {
    struct stat st;
    *out = OsStat{};
    if (stat(path, &st) != 0) { if (err) *err = errno; return false; }
    stat_to(st, out);
    return true;
}

// Linux and Android: the file system's magic number, from statfs. Those whose changes can
// come from another machine (or, for FUSE, from a process the kernel never sees write):
// the network file systems, and every FUSE mount, sshfs and rclone among them. fuseblk
// (ntfs-3g on a local disk) has FUSE's number too and is counted: polled for nothing,
// which is slow, never wrong. Apple: a volume the kernel does not call local, or macFUSE.
bool os_path_is_remote(const char* path) {
    struct statfs sf;
    if (statfs(path, &sf) != 0) return false;
#if defined(__APPLE__)
    if (!(sf.f_flags & MNT_LOCAL)) return true;
    return strncmp(sf.f_fstypename, "macfuse", 7) == 0 || strncmp(sf.f_fstypename, "osxfuse", 7) == 0 ||
           strncmp(sf.f_fstypename, "fuse", 4) == 0;
#else
    // As u32: f_type is a signed 32-bit word on some ABIs, and SMB's numbers have the top bit.
    switch ((u32)sf.f_type) {
    case 0x6969u:     // NFS
    case 0x517Bu:     // SMB (the old smbfs)
    case 0xFF534D42u: // CIFS
    case 0xFE534D42u: // SMB2 (the newer smb client)
    case 0x65735546u: // FUSE and fuseblk
    case 0x01021997u: // 9p (v9fs: WSL's drives, virtual machines' shares)
    case 0x5346414Fu: // AFS (OpenAFS)
    case 0x6B414653u: // kAFS
    case 0x73757245u: // Coda
    case 0x00C36400u: // Ceph
    case 0x564Cu:     // NCP
    case 0x0BD00BD0u: // Lustre
    case 0x20030528u: // OrangeFS
    case 0x01161970u: // GFS2, shared between machines
    case 0x7461636Fu: // OCFS2, likewise
        return true;
    default:
        return false;
    }
#endif
}

bool os_rename(const char* from, const char* to) { return rename(from, to) == 0; }

bool os_current_dir(char* out, u32 cap) { return getcwd(out, cap) != nullptr; }

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

namespace {
struct EachStat { const char* dir; void (*fn)(void*, const char*, const OsStat&); void* user; };
void each_stat_one(void* user, const char* name) {
    EachStat* c = (EachStat*)user;
    char path[4096];
    int n = format(path, sizeof path, "%s/%s", c->dir, name);
    if (n < 0 || (usize)n >= sizeof path) return;
    OsStat st;
    if (os_path_stat(path, &st)) c->fn(c->user, name, st);
}
} // namespace

// readdir does not carry sizes and times: a stat an entry, as a caller would have made.
bool os_dir_each_stat(const char* dir, void (*fn)(void* user, const char* name, const OsStat& st), void* user) {
    EachStat c{dir, fn, user};
    return os_dir_each(dir, each_stat_one, &c);
}

// readdir's d_type is the kind, where the file system fills it in: a directory of files
// lists with no stat at all unless sizes are asked. A link, or an entry typed
// DT_UNKNOWN (some network and older file systems), is asked of with fstatat against the
// open directory, which walks no path.
bool os_dir_list(const char* dir, bool sizes, void (*fn)(void* user, const OsDirEntry& e), void* user, i32* err) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) { if (err) *err = errno; return false; }
    DIR* d = fdopendir(fd);
    if (!d) { i32 e = errno; close(fd); if (err) *err = e; return false; }
    for (;;) {
        errno = 0;
        dirent* de = readdir(d);
        if (!de) break;
        const char* name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        OsDirEntry e;
        e.name   = name;
        e.hidden = name[0] == '.';
        unsigned char t = de->d_type;
        struct stat st;
        bool have = false; // st holds the entry's own stat
        if (t == DT_UNKNOWN) {
            if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue; // gone since the listing
            if (S_ISLNK(st.st_mode)) t = DT_LNK;
            else have = true;
        }
        if (have) {
            stat_to(st, &e.st);
        } else if (t == DT_LNK) {
            e.is_link = true;
            if (fstatat(fd, name, &st, 0) == 0) stat_to(st, &e.st); // else nothing at the far end: None
        } else if (sizes) {
            if (fstatat(fd, name, &st, 0) != 0) continue;
            stat_to(st, &e.st);
        } else {
            e.st.kind = t == DT_REG ? OsPathKind::File : t == DT_DIR ? OsPathKind::Dir : OsPathKind::Other;
        }
        if (!sizes) { e.st.size = 0; e.st.mtime_ns = 0; e.st.volume = 0; }
        fn(user, e);
    }
    i32 failed = errno;
    closedir(d);
    if (failed) { if (err) *err = failed; return false; }
    return true;
}

bool os_make_dir(const char* path) {
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

bool os_remove(const char* path) {
    return remove(path) == 0;   // ISO C: a file, or an empty directory on POSIX
}

bool os_remove_dir(const char* path) {
    return rmdir(path) == 0 || errno == ENOENT;
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
