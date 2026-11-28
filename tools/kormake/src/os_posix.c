/* kormake: the operating system on POSIX hosts (Linux, macOS). See os.h. */
#include "os.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utime.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

int main(int argc, char** argv) { return kormake_main(argc, argv); }

const char* kos_error(void) { return strerror(errno); }

FILE* kos_fopen(const char* path, const char* mode) { return fopen(path, mode); }

int kos_stat(const char* path, KosStat* out) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    out->is_dir = S_ISDIR(st.st_mode);
    out->is_file = S_ISREG(st.st_mode);
    out->mtime = (long long)st.st_mtime;
    out->size = (long long)st.st_size;
    return 0;
}

int kos_listdir(const char* path, void (*fn)(void* user, const char* name), void* user) {
    DIR* d = opendir(path);
    if (!d) return -1;
    struct dirent* e;
    while ((e = readdir(d)) != 0) fn(user, e->d_name);
    closedir(d);
    return 0;
}

int kos_mkdir(const char* path) {
    return mkdir(path, 0775) != 0 && errno != EEXIST ? -1 : 0;
}

int kos_rmtree(const char* path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path);
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != 0) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                char child[4096];
                snprintf(child, sizeof child, "%s/%s", path, e->d_name);
                kos_rmtree(child);
            }
            closedir(d);
        }
        return rmdir(path);
    }
    return unlink(path);
}

int kos_touch(const char* path) { return utime(path, NULL); }

int kos_rename(const char* from, const char* to) { return rename(from, to); }

int kos_link(const char* target, const char* path) { return symlink(target, path); }

int kos_readlink(const char* path, char* buf, size_t n) {
    ssize_t len = readlink(path, buf, n - 1);
    if (len < 0) return -1;
    buf[len] = 0;
    return 0;
}

long long kos_lock(const char* path) {
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    while (flock(fd, LOCK_EX) != 0) {
        if (errno != EINTR) { close(fd); return -1; }
    }
    return fd;
}

void kos_unlock(long long lock) {
    if (lock < 0) return;
    flock((int)lock, LOCK_UN);
    close((int)lock);
}

int kos_setenv(const char* name, const char* value) { return value ? setenv(name, value, 1) : unsetenv(name); }

int kos_copy(const char* from, const char* to) {
    int in = open(from, O_RDONLY);
    if (in < 0) return -1;
    struct stat st;
    if (fstat(in, &st) != 0) { close(in); return -1; }
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (out < 0) { close(in); return -1; }
    char buf[65536];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        for (ssize_t done = 0; done < n;) {
            ssize_t w = write(out, buf + done, (size_t)(n - done));
            if (w < 0) { if (errno == EINTR) continue; rc = -1; break; }
            done += w;
        }
        if (rc) break;
    }
    if (n < 0) rc = -1;
    if (close(out) != 0) rc = -1;
    close(in);
    return rc;
}

int kos_cwd(char* buf, size_t n) { return getcwd(buf, n) ? 0 : -1; }

int kos_realpath(const char* path, char* buf, size_t n) {
    char* r = realpath(path, NULL);
    if (!r) return -1;
    int fits = strlen(r) < n;
    if (fits) memcpy(buf, r, strlen(r) + 1); else errno = ENAMETOOLONG;
    free(r);
    return fits ? 0 : -1;
}

int kos_self(char* buf, size_t n) {
#if defined(__APPLE__)
    uint32_t size = (uint32_t)n;
    return _NSGetExecutablePath(buf, &size) == 0 ? 0 : -1;
#else
    ssize_t len = readlink("/proc/self/exe", buf, n - 1);
    if (len < 0) return -1;
    buf[len] = 0;
    return 0;
#endif
}

int kos_getenv(const char* name, char* buf, size_t n) {
    const char* v = getenv(name);
    if (!v || strlen(v) >= n) return 0;
    memcpy(buf, v, strlen(v) + 1);
    return 1;
}

int kos_exec(const char* cmd) {
    fflush(stdout);
    fflush(stderr);
    int rc = system(cmd);
    if (rc == -1) return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}
