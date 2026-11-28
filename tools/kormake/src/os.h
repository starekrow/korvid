/* kormake: what the host needs from the operating system. One implementation per host
 * family, chosen by the bootstrap: os_posix.c (Linux, macOS) and os_win32.c (Windows).
 * Paths in and out are UTF-8; paths handed back (cwd, realpath, self) use '/' as the
 * separator on every host, so the scripts see one form. Functions returning int give 0 on
 * success unless they say otherwise; kos_error() describes the last failure. */
#ifndef KORMAKE_OS_H
#define KORMAKE_OS_H

#include <stddef.h>
#include <stdio.h>

typedef struct {
    int       is_dir;
    int       is_file;
    long long mtime;   /* seconds since the Unix epoch */
    long long size;    /* bytes, for a file */
} KosStat;

/* The entry point both implementations call after preparing argv as UTF-8. */
int kormake_main(int argc, char** argv);

FILE*       kos_fopen(const char* path, const char* mode);
int         kos_stat(const char* path, KosStat* st);                 /* follows links */
int         kos_listdir(const char* path, void (*fn)(void* user, const char* name), void* user);
int         kos_mkdir(const char* path);                             /* 0 if made or already there */
int         kos_rmtree(const char* path);                            /* 0 if removed or absent */
int         kos_touch(const char* path);                             /* mtime to now */
int         kos_copy(const char* from, const char* to);              /* replaces; keeps exec bits */
int         kos_rename(const char* from, const char* to);            /* replaces to, in one step */
int         kos_cwd(char* buf, size_t n);
int         kos_realpath(const char* path, char* buf, size_t n);     /* fails if it does not exist */
int         kos_self(char* buf, size_t n);                           /* the running executable */
int         kos_getenv(const char* name, char* buf, size_t n);       /* 1 if set, 0 if not */
int         kos_exec(const char* cmd);                               /* the exit code, -1 if it could not run */
const char* kos_error(void);

/* A path separator: '/' everywhere, and '\\' as well on Windows. */
#if defined(_WIN32)
#define KOS_IS_SEP(c) ((c) == '/' || (c) == '\\')
#else
#define KOS_IS_SEP(c) ((c) == '/')
#endif

#endif
