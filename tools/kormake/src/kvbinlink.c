/* kvbinlink: the shim behind every linked tool (docs/PRDs/Kormake-Link-Tools-PRD.md). Each
 * name on the path (kpack, kormake, ...) leads here. The shim walks up from the current
 * directory to the korvid.config or kormake.lua kormake would find, reads that anchor's tool
 * table (which the engine's kormake writes, scripts/link.lua), brings the tool up to date
 * with Ninja and runs it. Outside any tree it runs the name's default, or the next program
 * of that name on PATH.
 *
 * It holds no Lua and reads no module file: what depends on the engine's version stays in
 * the engine's kormake, and the shim's contracts are the walk, the table's format, the
 * registry's and how to run a program. Those are versioned by KVBINLINK_PROTOCOL.
 *
 *   kpack ARGS                          as linked: the name is argv[0]'s
 *   kvbinlink NAME ARGS                    the same, by name
 *   NAME --diagnose-tool-lookup         what NAME would run from here, and why; runs nothing
 *   kvbinlink --version                    "kvbinlink <protocol>.<revision>"
 */
#if !defined(_WIN32)
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#define KVBINLINK_PROTOCOL 1 /* the walk, the table's major version, the registry, the environment */
#define KVBINLINK_REVISION 0 /* fixes within a protocol: a higher one replaces a lower */
#define TABLE_FORMAT 1

#if defined(__linux__)
#define HOST "linux"
#elif defined(__APPLE__)
#define HOST "macos"
#elif defined(_WIN32)
#define HOST "windows"
#else
#error "kvbinlink: unknown host platform"
#endif

#if defined(_WIN32)
#define EXE ".exe"
#define PATH_SEP ';'
#else
#define EXE ""
#define PATH_SEP ':'
#endif

#define PATH_CAP 4096
#define MAX_ROWS 256
#define MAX_WATCH 16
#define MAX_REG 256

static const char* g_name = "kvbinlink"; /* the tool's name, for messages */
static int g_diag = 0;                /* --diagnose-tool-lookup */
static int g_debug = 0;               /* KORVID_LINK_DEBUG */

/* ---- small things ---------------------------------------------------------------- */

static void say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", g_name);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void debug(const char* fmt, ...) {
    if (!g_debug) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "kvbinlink: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* The report --diagnose-tool-lookup prints: a label in a column, then the text. */
static void report(const char* label, const char* fmt, ...) {
    if (!g_diag) return;
    va_list ap;
    va_start(ap, fmt);
    printf("%-11s", label);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
}

static void copy(char* out, size_t cap, const char* s) {
    size_t n = strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

static void join(char* out, size_t cap, const char* a, const char* b) {
    size_t n = strlen(a);
    if (n > 0 && (a[n - 1] == '/' || a[n - 1] == '\\')) snprintf(out, cap, "%s%s", a, b);
    else snprintf(out, cap, "%s/%s", a, b);
}

static void dirname_of(char* out, size_t cap, const char* path) {
    copy(out, cap, path);
    char* slash = strrchr(out, '/');
#if defined(_WIN32)
    char* back = strrchr(out, '\\');
    if (back && (!slash || back > slash)) slash = back;
#endif
    if (!slash) { copy(out, cap, "."); return; }
    if (slash == out) slash[1] = 0; else *slash = 0;
}

static const char* basename_of(const char* path) {
    const char* b = path;
    for (const char* p = path; *p; ++p) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

static const char* env(const char* name) {
    const char* v = getenv(name);
    return v && *v ? v : NULL;
}

static int is_file(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int is_dir(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static long long file_mtime(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_mtime : -1;
}

static long long file_size(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_size : -1;
}

static int real(const char* path, char* out, size_t cap) {
#if defined(_WIN32)
    return _fullpath(out, path, cap) ? 0 : -1;
#else
    char* r = realpath(path, NULL);
    if (!r) return -1;
    copy(out, cap, r);
    free(r);
    return 0;
#endif
}

static int self_path(char* out, size_t cap) {
#if defined(__APPLE__)
    char buf[PATH_CAP];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return -1;
    return real(buf, out, cap);
#elif defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    return n > 0 && n < cap ? 0 : -1;
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n < 0) return -1;
    out[n] = 0;
    return 0;
#endif
}

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)(n > 0 ? n : 0) + 1);
    size_t got = n > 0 ? fread(buf, 1, (size_t)n, f) : 0;
    buf[got] = 0;
    fclose(f);
    return buf;
}

/* FNV-1a, 64 bits: a table's file name (scripts/link.lua's hash). */
static unsigned long long fnv1a(const char* s) {
    unsigned long long h = 0xcbf29ce484222325ULL;
    for (; *s; ++s) { h ^= (unsigned char)*s; h *= 0x100000001b3ULL; }
    return h;
}

/* Splits a line at tabs in place: up to cap fields; the count. */
static int split_tabs(char* line, char** fields, int cap) {
    int n = 0;
    fields[n++] = line;
    for (char* p = line; *p && n < cap; ++p) {
        if (*p == '\t') { *p = 0; fields[n++] = p + 1; }
    }
    return n;
}

/* ---- where things are ------------------------------------------------------------ */

static char g_self[PATH_CAP];  /* the shim, by its real path */
static char g_home[PATH_CAP];  /* the link home: where the shim and the registry are */
static char g_cache[PATH_CAP]; /* the tool tables */

static void find_places(void) {
    if (self_path(g_self, sizeof g_self) != 0) copy(g_self, sizeof g_self, "");
    const char* h = env("KORVID_LINK_HOME");
    if (h) copy(g_home, sizeof g_home, h); else dirname_of(g_home, sizeof g_home, g_self);
    const char* c = env("KORVID_LINK_CACHE");
    if (c) { copy(g_cache, sizeof g_cache, c); return; }
#if defined(_WIN32)
    const char* base = env("LOCALAPPDATA");
    snprintf(g_cache, sizeof g_cache, "%s/korvid-link/cache", base ? base : ".");
#elif defined(__APPLE__)
    const char* home = env("HOME");
    snprintf(g_cache, sizeof g_cache, "%s/Library/Caches/korvid-link", home ? home : ".");
#else
    const char* xdg = env("XDG_CACHE_HOME");
    if (xdg) snprintf(g_cache, sizeof g_cache, "%s/korvid-link", xdg);
    else snprintf(g_cache, sizeof g_cache, "%s/.cache/korvid-link", env("HOME") ? env("HOME") : ".");
#endif
}

/* ---- the registry: what `kormake link tools` vouched for ------------------------- */

typedef struct { char kind[16]; char a[PATH_CAP]; char b[PATH_CAP]; } RegEntry;
static RegEntry g_reg[MAX_REG];
static int g_nreg = 0;

static void load_registry(void) {
    char path[PATH_CAP];
    join(path, sizeof path, g_home, "registry.txt");
    char* text = read_file(path);
    if (!text) return;
    char* save = NULL;
    for (char* line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        char* f[4];
        int n = split_tabs(line, f, 4);
        if (n < 2 || g_nreg >= MAX_REG) continue;
        RegEntry* e = &g_reg[g_nreg++];
        copy(e->kind, sizeof e->kind, f[0]);
        copy(e->a, sizeof e->a, f[1]);
        copy(e->b, sizeof e->b, n > 2 ? f[2] : "");
    }
    free(text);
}

static int registered(const char* kind, const char* path) {
    for (int i = 0; i < g_nreg; ++i) if (strcmp(g_reg[i].kind, kind) == 0 && strcmp(g_reg[i].a, path) == 0) return 1;
    return 0;
}

static const char* default_for(const char* name) {
    for (int i = g_nreg - 1; i >= 0; --i) if (strcmp(g_reg[i].kind, "default") == 0 && strcmp(g_reg[i].a, name) == 0) return g_reg[i].b;
    return NULL;
}

static void kormake_of(char* out, size_t cap, const char* engine) {
    snprintf(out, cap, "%s/tools/kormake/bin/%s/kormake%s", engine, HOST, EXE);
}

/* The most recently linked engine whose kormake is built: who reads a config first. */
static const char* any_engine(void) {
    for (int i = g_nreg - 1; i >= 0; --i) {
        if (strcmp(g_reg[i].kind, "engine") != 0) continue;
        char k[PATH_CAP];
        kormake_of(k, sizeof k, g_reg[i].a);
        if (is_file(k)) return g_reg[i].a;
    }
    return NULL;
}

/* ---- the tool table -------------------------------------------------------------- */

typedef struct {
    int  shadowed;
    char name[128], from[16], module[128], by[160];
    char build[PATH_CAP], target[128], exe[PATH_CAP], unavailable[512], args[PATH_CAP];
} Row;

typedef struct {
    int  loaded, format;
    char path[PATH_CAP];
    long long mtime;
    char anchor[PATH_CAP], engine[PATH_CAP], project[PATH_CAP], kormake[PATH_CAP], ninja[PATH_CAP];
    int  nwatch;
    struct { char path[PATH_CAP]; long long size, mtime; } watch[MAX_WATCH];
    int  nrows;
    Row  rows[MAX_ROWS];
} Table;

static Table g_table;

static void table_path(char* out, size_t cap, const char* anchor) {
    if (snprintf(out, cap, "%s/roots/%016llx.txt", g_cache, fnv1a(anchor)) >= (int)cap) out[0] = 0;
}

static void field(Row* r, const char* kv) {
    const char* eq = strchr(kv, '=');
    if (!eq) return;
    size_t k = (size_t)(eq - kv);
    const char* v = eq + 1;
#define F(key, dst) if (k == strlen(key) && strncmp(kv, key, k) == 0) { copy(dst, sizeof dst, v); return; }
    F("from", r->from) F("module", r->module) F("by", r->by) F("build", r->build) F("target", r->target)
    F("exe", r->exe) F("unavailable", r->unavailable) F("args", r->args)
#undef F
}

/* Reads the anchor's table. 0: read; -1: none; -2: a newer format than this shim reads. */
static int load_table(const char* anchor) {
    Table* t = &g_table;
    memset(t, 0, sizeof *t);
    table_path(t->path, sizeof t->path, anchor);
    char* text = read_file(t->path);
    if (!text) return -1;
    t->mtime = file_mtime(t->path);
    char* save = NULL;
    int first = 1;
    for (char* line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        char* f[16];
        int n = split_tabs(line, f, 16);
        if (first) {
            first = 0;
            if (n < 2 || strcmp(f[0], "korvid-link") != 0) { free(text); return -1; }
            t->format = atoi(f[1]);
            if (t->format > TABLE_FORMAT) { free(text); return -2; }
            continue;
        }
        if (n >= 2 && strcmp(f[0], "anchor") == 0) copy(t->anchor, sizeof t->anchor, f[1]);
        else if (n >= 2 && strcmp(f[0], "engine") == 0) copy(t->engine, sizeof t->engine, f[1]);
        else if (n >= 2 && strcmp(f[0], "project") == 0) copy(t->project, sizeof t->project, f[1]);
        else if (n >= 2 && strcmp(f[0], "kormake") == 0) copy(t->kormake, sizeof t->kormake, f[1]);
        else if (n >= 2 && strcmp(f[0], "ninja") == 0) copy(t->ninja, sizeof t->ninja, f[1]);
        else if (n >= 4 && strcmp(f[0], "watch") == 0 && t->nwatch < MAX_WATCH) {
            copy(t->watch[t->nwatch].path, PATH_CAP, f[1]);
            t->watch[t->nwatch].size = atoll(f[2]);
            t->watch[t->nwatch].mtime = atoll(f[3]);
            t->nwatch++;
        } else if (n >= 2 && (strcmp(f[0], "tool") == 0 || strcmp(f[0], "shadowed") == 0) && t->nrows < MAX_ROWS) {
            Row* r = &t->rows[t->nrows++];
            memset(r, 0, sizeof *r);
            r->shadowed = f[0][0] == 's';
            copy(r->name, sizeof r->name, f[1]);
            for (int i = 2; i < n; ++i) field(r, f[i]);
        }
        /* Any other line is a later engine's: skipped. */
    }
    free(text);
    t->loaded = strcmp(t->anchor, anchor) == 0;
    return t->loaded ? 0 : -1;
}

/* Why the loaded table no longer says what the anchor is, or NULL if it still does. */
static const char* table_stale(void) {
    static char why[PATH_CAP + 64];
    for (int i = 0; i < g_table.nwatch; ++i) {
        const char* p = g_table.watch[i].path;
        if (file_size(p) != g_table.watch[i].size || file_mtime(p) != g_table.watch[i].mtime) {
            snprintf(why, sizeof why, "%s changed", p);
            return why;
        }
    }
    return NULL;
}

static Row* find_row(const char* name) {
    for (int i = 0; i < g_table.nrows; ++i) if (!g_table.rows[i].shadowed && strcmp(g_table.rows[i].name, name) == 0) return &g_table.rows[i];
    return NULL;
}

static void published_names(char* out, size_t cap) {
    out[0] = 0;
    for (int i = 0; i < g_table.nrows; ++i) {
        if (g_table.rows[i].shadowed) continue;
        size_t n = strlen(out);
        snprintf(out + n, cap - n, "%s%s", n ? ", " : "", g_table.rows[i].name);
    }
}

/* ---- running things -------------------------------------------------------------- */

#define OUT_TO_ERR 1 /* the child's standard output goes to our standard error */
#define OUT_CAPTURE 2

/* Runs argv and waits: its exit status, 127 if it could not start. With OUT_CAPTURE its
 * standard output (the first cap - 1 bytes) is put in out. */
static int run_child(char* const* argv, int mode, char* out, size_t cap) {
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32)
    (void)argv; (void)mode; (void)out; (void)cap;
    return 127; /* milestone 3 */
#else
    int fds[2] = { -1, -1 };
    if (mode == OUT_CAPTURE && pipe(fds) != 0) return 127;
    pid_t pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        if (mode == OUT_CAPTURE) { dup2(fds[1], 1); close(fds[0]); close(fds[1]); }
        else if (mode == OUT_TO_ERR) dup2(2, 1);
        execvp(argv[0], argv);
        fprintf(stderr, "%s: cannot run %s: %s\n", g_name, argv[0], strerror(errno));
        _exit(127);
    }
    if (mode == OUT_CAPTURE) {
        close(fds[1]);
        size_t got = 0;
        ssize_t n;
        char sink[256];
        while ((n = read(fds[0], got + 1 < cap ? out + got : sink, got + 1 < cap ? cap - 1 - got : sizeof sink)) > 0) {
            if (got + 1 < cap) got += (size_t)n;
        }
        out[got < cap ? got : cap - 1] = 0;
        close(fds[0]);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return 127;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#endif
}

/* Replaces this process with exe, given argv (argv[0] included). Returns only on failure. */
static void run_instead(const char* exe, char* const* argv) {
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32)
    (void)exe; (void)argv; /* milestone 3 */
#else
    execv(exe, argv);
#endif
}

static long long lock_build(const char* build) {
    char path[PATH_CAP];
    join(path, sizeof path, build, ".korvid.lock");
#if defined(_WIN32)
    (void)path;
    return -1; /* milestone 3 */
#else
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    while (flock(fd, LOCK_EX) != 0) if (errno != EINTR) { close(fd); return -1; }
    return fd;
#endif
}

static void unlock_build(long long lock) {
    if (lock < 0) return;
#if !defined(_WIN32)
    flock((int)lock, LOCK_UN);
    close((int)lock);
#endif
}

/* ---- the walk -------------------------------------------------------------------- */

typedef struct { char dir[PATH_CAP]; int kind; } Anchor; /* kind: 1 korvid.config, 2 kormake.lua */

static int walk(const char* start, Anchor* a) {
    char dir[PATH_CAP];
    if (real(start, dir, sizeof dir) != 0) { say("no directory '%s'", start); return 0; }
    int first = 1;
    for (;;) {
        char cfg[PATH_CAP], master[PATH_CAP];
        join(cfg, sizeof cfg, dir, "korvid.config");
        join(master, sizeof master, dir, "kormake.lua");
        int kind = is_file(cfg) ? 1 : is_file(master) ? 2 : 0;
        report(first ? "walk" : "", "%s: %s", dir, kind == 1 ? "korvid.config  <- anchor" : kind == 2 ? "kormake.lua  <- anchor" : "nothing");
        first = 0;
        if (kind) { copy(a->dir, sizeof a->dir, dir); a->kind = kind; return 1; }
        char parent[PATH_CAP];
        dirname_of(parent, sizeof parent, dir);
        if (strcmp(parent, dir) == 0 || parent[0] == 0 || strcmp(parent, ".") == 0) return 0;
#if defined(_WIN32)
        if (strlen(dir) <= 3 && dir[1] == ':') return 0;
#endif
        copy(dir, sizeof dir, parent);
    }
}

/* ---- PATH: the programs this name means outside a tree --------------------------- */

#define MAX_PATHS 32
static char g_path_hits[MAX_PATHS][PATH_CAP];
static int g_npath = 0;

static void search_path(const char* name) {
    const char* p = env("PATH");
    if (!p) return;
    char self_real[PATH_CAP];
    if (real(g_self, self_real, sizeof self_real) != 0) copy(self_real, sizeof self_real, g_self);
    while (*p && g_npath < MAX_PATHS) {
        const char* end = strchr(p, PATH_SEP);
        size_t n = end ? (size_t)(end - p) : strlen(p);
        char dir[PATH_CAP], cand[PATH_CAP], cr[PATH_CAP];
        if (n > 0 && n < sizeof dir) {
            memcpy(dir, p, n);
            dir[n] = 0;
            char file[256];
            snprintf(file, sizeof file, "%s%s", name, EXE);
            join(cand, sizeof cand, dir, file);
            if (is_file(cand) && real(cand, cr, sizeof cr) == 0 && strcmp(cr, self_real) != 0
#if !defined(_WIN32)
                && access(cand, X_OK) == 0
#endif
            ) {
                int dup = 0;
                for (int i = 0; i < g_npath; ++i) if (strcmp(g_path_hits[i], cand) == 0) dup = 1;
                if (!dup) copy(g_path_hits[g_npath++], PATH_CAP, cand);
            }
        }
        if (!end) break;
        p = end + 1;
    }
}

/* ---- the shim -------------------------------------------------------------------- */

static int g_argc;
static char** g_argv; /* the tool's arguments, argv[0] its name */

/* Outside any tree, or in one not vouched for: the next program of this name on PATH. */
static int fall_through(const char* why) {
    search_path(g_name);
    if (g_diag) {
        printf("candidates\n");
        for (int i = 0; i < g_npath; ++i) printf("  %d PATH     %s\n", i + 1, g_path_hits[i]);
        if (g_npath == 0) printf("  (none)\n");
        if (g_npath > 0) report("chosen", "1: %s, so the first other %s on PATH", why, g_name);
        else report("chosen", "nothing: %s, and there is no other %s on PATH", why, g_name);
        return g_npath > 0 ? 0 : 127;
    }
    if (g_npath == 0) { say("%s", why); return 127; }
    debug("falling through to %s", g_path_hits[0]);
    run_instead(g_path_hits[0], g_argv);
    say("cannot run %s: %s", g_path_hits[0], strerror(errno));
    return 126;
}

/* kormake's own: KORMAKE `_link VERB DIR`, its standard output captured or kept off ours. */
static int link_verb(const char* kormake, const char* verb, const char* dir, char* out, size_t cap) {
    char* argv[] = { (char*)kormake, (char*)"_link", (char*)verb, (char*)dir, NULL };
    debug("%s _link %s %s", kormake, verb, dir);
    return run_child(argv, out ? OUT_CAPTURE : OUT_TO_ERR, out, cap);
}

/* Asks the engine's kormake to write the anchor's table, and reads it. */
static int rewrite_table(const char* engine, const char* anchor) {
    char k[PATH_CAP];
    kormake_of(k, sizeof k, engine);
    if (!is_file(k)) {
        say("%s has no kormake built; run %s/tools/kormake/bootstrap.%s", engine, engine,
#if defined(_WIN32)
            "bat"
#else
            "sh"
#endif
        );
        return -1;
    }
    char out[PATH_CAP];
    if (link_verb(k, "write", anchor, out, sizeof out) != 0) { say("%s could not write the tool table for %s (above)", k, anchor); return -1; }
    int rc = load_table(anchor);
    if (rc == -2) { say("%s writes a newer tool table than this shim reads; run kormake link tools from that engine", engine); return -1; }
    if (rc != 0) { say("%s wrote no tool table for %s", k, anchor); return -1; }
    return 0;
}

static void print_row(int i, const char* side, const Row* r, const char* state) {
    if (!g_diag) return;
    printf("  %d %-8s %s%s%s\n", i, side, r->exe[0] ? r->exe : r->unavailable[0] ? "(cannot build here)" : "(not built here)",
           state[0] ? "   " : "", state);
}

/* Whether a build would do anything for the row: a dry Ninja run. */
static const char* rebuild_state(const Row* r) {
    static char state[64];
    if (!r->build[0]) return "";
    char bn[PATH_CAP];
    join(bn, sizeof bn, r->build, "build.ninja");
    if (!is_file(bn)) return "not generated yet";
    char out[4096];
    char* argv[] = { g_table.ninja[0] ? g_table.ninja : (char*)"ninja", (char*)"-n", (char*)"-C", (char*)r->build, (char*)r->target, NULL };
    int rc = run_child(argv, OUT_CAPTURE, out, sizeof out);
    if (rc != 0) snprintf(state, sizeof state, "a dry Ninja run failed (%d)", rc);
    else if (strstr(out, "no work to do")) snprintf(state, sizeof state, "%s, up to date", is_file(r->exe) ? "built" : "not built");
    else snprintf(state, sizeof state, "%s, rebuild due", is_file(r->exe) ? "built" : "not built");
    return state;
}

static int usage(void) {
    fprintf(stderr, "usage: kvbinlink NAME [ARGS...]   (or run it as NAME, through a link)\n"
                    "       NAME --diagnose-tool-lookup\n"
                    "       kvbinlink --version\n"
                    "See docs/PRDs/Kormake-Link-Tools-PRD.md and tools/kormake/README.md.\n");
    return 2;
}

int main(int argc, char** argv) {
    static char name[256];
    copy(name, sizeof name, basename_of(argv[0]));
    size_t nl = strlen(name);
    if (nl > 4 && (strcmp(name + nl - 4, ".exe") == 0 || strcmp(name + nl - 4, ".EXE") == 0)) name[nl - 4] = 0;
    char** args = argv;
    int nargs = argc;
    if (strcmp(name, "kvbinlink") == 0) {
        if (argc < 2) return usage();
        if (strcmp(argv[1], "--version") == 0) { printf("kvbinlink %d.%d\n", KVBINLINK_PROTOCOL, KVBINLINK_REVISION); return 0; }
        if (strcmp(argv[1], "--help") == 0 || argv[1][0] == '-') return usage();
        copy(name, sizeof name, argv[1]);
        args = argv + 1;
        nargs = argc - 1;
    }
    g_name = name;
    if (nargs > 1 && strcmp(args[1], "--diagnose-tool-lookup") == 0) {
        g_diag = 1;
        args[1] = args[0]; /* drop it: the rest are the tool's */
        args++;
        nargs--;
    }
    g_argc = nargs;
    g_argv = args;
    g_debug = env("KORVID_LINK_DEBUG") != NULL;

    /* A shim that finds itself again (a table pointing at a link) stops. */
    const char* depth_s = env("KORVID_LINK_DEPTH");
    int depth = depth_s ? atoi(depth_s) : 0;
    if (depth > 8) { say("runs itself; a tool table or PATH leads back to the shim"); return 1; }
    char depth_v[16];
    snprintf(depth_v, sizeof depth_v, "%d", depth + 1);
#if defined(_WIN32)
    _putenv_s("KORVID_LINK_DEPTH", depth_v);
#else
    setenv("KORVID_LINK_DEPTH", depth_v, 1);
#endif

    find_places();
    load_registry();

    /* 1. The walk: from here, from KORVID_LINK_DIR, or for kormake from its -C DIR. */
    char cwd[PATH_CAP];
#if defined(_WIN32)
    if (!_getcwd(cwd, sizeof cwd)) copy(cwd, sizeof cwd, ".");
#else
    if (!getcwd(cwd, sizeof cwd)) copy(cwd, sizeof cwd, ".");
#endif
    char start[PATH_CAP];
    copy(start, sizeof start, env("KORVID_LINK_DIR") ? env("KORVID_LINK_DIR") : cwd);
    if (strcmp(name, "kormake") == 0) {
        for (int i = 1; i + 1 < nargs; ++i) {
            if (strcmp(args[i], "-C") == 0) {
                const char* d = args[i + 1];
                if (d[0] == '/' || (d[0] && d[1] == ':')) copy(start, sizeof start, d); else join(start, sizeof start, cwd, d);
                break;
            }
        }
    }
    report("cwd", "%s", cwd);
    if (strcmp(start, cwd) != 0) report("start", "%s (%s)", start, env("KORVID_LINK_DIR") ? "KORVID_LINK_DIR" : "-C");
    report("shim", "%s (kvbinlink %d.%d), registry %s/registry.txt", g_self, KVBINLINK_PROTOCOL, KVBINLINK_REVISION, g_home);

    Anchor a;
    memset(&a, 0, sizeof a);
    int found = walk(start, &a);
    if (!found) {
        const char* d = default_for(name);
        if (d && is_dir(d)) {
            report("default", "%s (kormake link default)", d);
            copy(a.dir, sizeof a.dir, d);
            char cfg[PATH_CAP];
            join(cfg, sizeof cfg, d, "korvid.config");
            a.kind = is_file(cfg) ? 1 : 2;
            found = 1;
        } else {
            report("context", "none: no korvid.config or kormake.lua above %s", start);
            char why[PATH_CAP + 96];
            snprintf(why, sizeof why, "not inside a Korvid engine or project (no korvid.config or kormake.lua above %s)", start);
            return fall_through(why);
        }
    }

    /* 2. The engine: the anchor itself, the table's word, or a linked kormake's reading of
     * the config (sandboxed: nothing of the anchor's runs before the trust check). */
    char engine[PATH_CAP] = "";
    int trc = load_table(a.dir);
    const char* stale = trc == 0 ? table_stale() : NULL;
    if (a.kind == 2) copy(engine, sizeof engine, a.dir);
    else if (trc == 0 && !stale) copy(engine, sizeof engine, g_table.engine);
    else {
        const char* e = any_engine();
        if (!e) {
            report("context", "%s, whose engine no linked kormake has read", a.dir);
            return fall_through("no engine is linked yet (run kormake link tools in one)");
        }
        char k[PATH_CAP], out[PATH_CAP];
        kormake_of(k, sizeof k, e);
        if (link_verb(k, "engine", a.dir, out, sizeof out) != 0) { say("%s could not read %s/korvid.config (above)", k, a.dir); return 1; }
        char* nlp = strpbrk(out, "\r\n");
        if (nlp) *nlp = 0;
        copy(engine, sizeof engine, out);
    }

    /* 3. Trust: the engine, and a project, were linked by kormake link tools. */
    int engine_ok = registered("engine", engine);
    if (!engine_ok) {
        report("context", "%s, engine %s", a.dir, engine);
        report("trust", "engine not linked");
        char why[PATH_CAP * 2];
        snprintf(why, sizeof why, "%s is not linked (run kormake link tools there to use its tools)", engine);
        return fall_through(why);
    }

    /* kormake is the engine's own binary: no table, no build. */
    if (strcmp(name, "kormake") == 0) {
        char k[PATH_CAP];
        kormake_of(k, sizeof k, engine);
        report("context", "%s %s, engine %s", a.kind == 2 ? "engine root" : "config", a.dir, engine);
        report("trust", "engine linked");
        if (g_diag) {
            search_path(name);
            printf("candidates\n  1 engine   %s   %s\n", k, is_file(k) ? "exists" : "missing: run the engine's bootstrap");
            for (int i = 0; i < g_npath; ++i) printf("  %d PATH     %s\n", i + 2, g_path_hits[i]);
            report("chosen", "1: kormake is the engine's own (tools/kormake/bin/%s), not a module; PATH programs are used only outside a linked tree", HOST);
            return is_file(k) ? 0 : 127;
        }
        if (!is_file(k)) { say("%s is not built; run %s/tools/kormake/bootstrap.sh", k, engine); return 126; }
        args[0] = k;
        run_instead(k, args);
        say("cannot run %s: %s", k, strerror(errno));
        return 126;
    }

    /* 4. The table, written again when it is missing, stale or lacks the name. */
    const char* why_rewrite = trc == -1 ? "missing" : trc == -2 ? "a newer format" : stale ? stale : NULL;
    if (!why_rewrite && !find_row(name)) why_rewrite = "has no row for this name";
    if (why_rewrite) {
        debug("tool table %s: %s", g_table.path, why_rewrite);
        if (rewrite_table(engine, a.dir) != 0) return 1;
    }
    const char* kind = g_table.project[0] ? "project" : a.kind == 2 ? "engine root" : "in-tree config";
    report("context", "%s %s, engine %s", kind, a.dir, engine);
    report("table", "%s (%s%s)", g_table.path, why_rewrite ? "rewritten: " : "current", why_rewrite ? why_rewrite : "");
    report("kormake", "%s", g_table.kormake);
    int project_ok = !g_table.project[0] || registered("project", g_table.project);
    report("trust", "engine linked%s", g_table.project[0] ? (project_ok ? ", project linked" : ", project NOT linked") : "");
    if (!project_ok) {
        char why[PATH_CAP * 2];
        snprintf(why, sizeof why, "%s is not linked (run kormake link tools there to use its tools)", g_table.project);
        return fall_through(why);
    }

    Row* row = find_row(name);
    if (g_diag) {
        search_path(name);
        printf("candidates\n");
        int i = 0, chosen = 0;
        for (int j = 0; j < g_table.nrows; ++j) {
            Row* r = &g_table.rows[j];
            if (strcmp(r->name, name) != 0) continue;
            char side[160];
            snprintf(side, sizeof side, "%s", r->from);
            ++i;
            if (r == row) chosen = i;
            char state[640];
            if (r->unavailable[0]) snprintf(state, sizeof state, "cannot build here: %s", r->unavailable);
            else if (r->shadowed) snprintf(state, sizeof state, "(module %s: lost to %s)", r->module, r->by);
            else snprintf(state, sizeof state, "%s", rebuild_state(r));
            print_row(i, side, r, state);
        }
        int first_path = i + 1;
        for (int j = 0; j < g_npath; ++j) printf("  %d PATH     %s\n", ++i, g_path_hits[j]);
        if (i == 0) printf("  (none)\n");
        (void)first_path;
        if (!row) {
            char names[2048];
            published_names(names, sizeof names);
            report("chosen", "nothing: %s is not published here (this context publishes %s); PATH programs are used only outside a linked tree", name, names);
            return 127;
        }
        int has_shadow = 0;
        for (int j = 0; j < g_table.nrows; ++j) if (g_table.rows[j].shadowed && strcmp(g_table.rows[j].name, name) == 0) has_shadow = 1;
        if (strcmp(row->from, "project") == 0)
            report("chosen", "%d: project module %s publishes \"%s\"%s; PATH programs are used only outside a linked tree", chosen, row->module, name,
                   has_shadow ? ", and a project's command wins over the engine's" : "");
        else
            report("chosen", "%d: engine module %s publishes \"%s\"%s; PATH programs are used only outside a linked tree", chosen, row->module, name,
                   has_shadow ? ", and it keeps its name" : "");
        if (row->unavailable[0]) { report("", "but it cannot build here: %s", row->unavailable); return 126; }
        return 0;
    }
    if (!row) {
        char names[2048];
        published_names(names, sizeof names);
        say("not published here (%s %s publishes %s)", kind, a.dir, names);
        return 127;
    }
    if (row->unavailable[0]) { say("cannot be built here: %s", row->unavailable); return 126; }

    /* 5. Up to date: Ninja, holding the build directory's lock, its output off ours. A
     * build that holds the lock already (a cook that calls this tool) is left to it. */
    const char* policy = env("KORVID_LINK_BUILD");
    const char* held = env("KORVID_LINK_HELD");
    int build = row->build[0] && !(policy && strcmp(policy, "never") == 0) && !(held && strcmp(held, row->build) == 0);
    if (build) {
        char bn[PATH_CAP];
        join(bn, sizeof bn, row->build, "build.ninja");
        if (!is_file(bn)) {
            if (rewrite_table(engine, a.dir) != 0) return 1;
            row = find_row(name);
            if (!row) { say("not published here any more"); return 127; }
        }
        long long lock = lock_build(row->build);
        char* nargv[] = { g_table.ninja[0] ? g_table.ninja : (char*)"ninja", (char*)"--quiet", (char*)"-C", row->build, row->target, NULL };
        debug("%s --quiet -C %s %s", nargv[0], row->build, row->target);
        int rc = run_child(nargv, OUT_TO_ERR, NULL, 0);
        unlock_build(lock);
        if (rc != 0) { say("rebuilding failed (KORVID_LINK_BUILD=never runs the binary as it is)"); return rc; }
        /* A regeneration wrote the table again: read the row again. */
        if (file_mtime(g_table.path) != g_table.mtime) {
            char keep[128];
            copy(keep, sizeof keep, name);
            if (load_table(a.dir) != 0 || !(row = find_row(keep))) { say("not published here any more"); return 127; }
        }
    }

    /* 6. Run it, in this process's place. */
    if (!is_file(row->exe)) { say("%s is not built%s", row->exe, build ? "" : " (KORVID_LINK_BUILD=never)"); return 126; }
#if defined(_WIN32)
    _putenv_s("KORVID_LINK_NAME", name);
#else
    setenv("KORVID_LINK_NAME", name, 1);
#endif
    debug("running %s", row->exe);
    args[0] = row->exe;
    run_instead(row->exe, args);
    say("cannot run %s: %s", row->exe, strerror(errno));
    return 126;
}
