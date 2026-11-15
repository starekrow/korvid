// goldberg, heavy: a synthetic cook big enough to behave like a real one. A tree of files
// full of seeded noise in a spread of size classes (a few of a fifth of the tree each, down
// to tens of thousands of a few kilobytes), the OS's cache dropped, then cooked by the
// built goldberg through a shell converter that does work nothing can skip: every 32-byte
// block replaced by its SHA-256 (or, with --work swap, every pair of bytes swapped). Then a
// cook with nothing to do, and one with a file per class changed. It reports what each
// phase took and, from what the converter recorded of every run, what each size class
// took, so the numbers can be put beside another machine's.
//
//   kormake test --heavy goldberg [-- OPTIONS]
//   build/<target>/bin/goldberg_heavy [OPTIONS]
//
// The same bytes on every platform: the tree, what it cooks to and the digest printed at
// the end depend on --size, --classes, --seed and --work alone. It fails on a cook that
// fails or an output that is wrong, never on a time.
//
// This executable is also the converter: `goldberg_heavy xform MODE IN OUT TIMES` is what
// the generated script runs, once per file.
#include "korvid/core/array.h"
#include "korvid/core/atomic.h"
#include "korvid/core/job.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/sha256.h"
#include "korvid/core/thread.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/platform/process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/vfs.h>
#else
#include <sys/mount.h>
#endif
#endif

namespace {

using namespace kv;

#if defined(_WIN32)
const char* kExe = ".exe";
#else
const char* kExe = "";
#endif
const u32 kSteps = 8; // goldberg runs this many converters at once (GoldbergState::max_procs)

// ---- the bytes --------------------------------------------------------------------------------

u64 mix(u64 z) {
    z += 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// A file's bytes [at, at + n): word i of the file is mix(seed + i), little-endian, so any
// range can be made on its own and every platform makes the same ones.
void fill(u8* out, usize n, u64 seed, u64 at) {
    u64 word = at / 8;
    u32 skip = (u32)(at % 8);
    usize i = 0;
    while (i < n) {
        u64 v = mix(seed + word++);
        for (u32 b = skip; b < 8 && i < n; ++b) out[i++] = (u8)(v >> (8 * b));
        skip = 0;
    }
}

// The cook: in place of `in`, into `out` (the same size).
void transform(bool hash, const u8* in, u8* out, usize n) {
    if (hash) {
        u8 d[32];
        for (usize at = 0; at < n; at += 32) {
            usize len = n - at < 32 ? n - at : 32;
            sha256(in + at, len, d);
            memcpy(out + at, d, len);
        }
    } else {
        usize i = 0;
        for (; i + 1 < n; i += 2) { out[i] = in[i + 1]; out[i + 1] = in[i]; }
        if (i < n) out[i] = in[i];
    }
}

// ---- the converter ----------------------------------------------------------------------------

const char* base_name(const char* path) {
    const char* b = path;
    for (const char* p = path; *p; ++p) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

// xform MODE IN OUT TIMES: cooks IN into OUT and writes what it took to TIMES/<stem>, as
// "bytes start read worked written cpu": the clock (time_now, one clock for every process
// on the machine) at its start and after each part, and its CPU seconds.
int xform(int argc, char** argv) {
    if (argc != 6) { fprintf(stderr, "usage: goldberg_heavy xform hash|swap IN OUT TIMES\n"); return 2; }
    f64 t0 = time_now();
    u64 c0 = os_thread_cpu_ticks();
    bool hash = strcmp(argv[2], "hash") == 0;
    IoBytes in;
    i32 err = 0;
    if (!io_read_now(argv[3], &in, nullptr, &err)) { fprintf(stderr, "cannot read %s (errno %d)\n", argv[3], err); return 1; }
    f64 t1 = time_now();
    u8* out = (u8*)mem_alloc(in.size ? in.size : 1);
    transform(hash, in.data, out, in.size);
    f64 t2 = time_now();
    bool ok = io_write_now(argv[4], out, in.size, &err);
    f64 t3 = time_now();
    mem_free(out);
    usize size = in.size;
    in.free();
    if (!ok) { fprintf(stderr, "cannot write %s (errno %d)\n", argv[4], err); return 1; }
    f64 cpu = (f64)(os_thread_cpu_ticks() - c0) / (f64)os_ticks_per_second();
    char stem[256], rec[700], line[200];
    snprintf(stem, sizeof stem, "%s", base_name(argv[4]));
    if (char* dot = strchr(stem, '.')) *dot = 0;
    snprintf(rec, sizeof rec, "%s/%s", argv[5], stem);
    int len = snprintf(line, sizeof line, "%llu %.9f %.9f %.9f %.9f %.9f\n", (unsigned long long)size, t0, t1, t2, t3, cpu);
    io_write_now(rec, line, (usize)len);
    return 0;
}

// ---- the machine ------------------------------------------------------------------------------

// Drops a file's pages from the OS's cache, as far as an unprivileged process can.
// Linux: written back, then DONTNEED. macOS has no fadvise: written back, then a shared
// mapping invalidated. Windows: opening a file unbuffered has the cache manager flush and
// purge what it holds of it.
bool evict(const char* path) {
#if defined(_WIN32)
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    bool ok = true;
#if defined(__linux__)
    fdatasync(fd);
    ok = posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) == 0;
#else
    fsync(fd);
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
        void* p = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        if (p != MAP_FAILED) { ok = msync(p, (size_t)st.st_size, MS_INVALIDATE) == 0; munmap(p, (size_t)st.st_size); }
    }
#endif
    close(fd);
    return ok;
#endif
}

// Of a file's pages, how many are in memory, and how many it has. False where the OS will
// not say (Windows).
bool resident(const char* path, u64* in, u64* pages) {
#if defined(_WIN32)
    (void)path; (void)in; (void)pages;
    return false;
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0;
    if (ok && st.st_size > 0) {
        usize page = (usize)sysconf(_SC_PAGESIZE);
        usize n = ((usize)st.st_size + page - 1) / page;
        void* p = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) ok = false;
        else {
#if defined(__linux__)
            unsigned char* v = (unsigned char*)mem_alloc(n);
#else
            char* v = (char*)mem_alloc(n);
#endif
            ok = mincore(p, (size_t)st.st_size, v) == 0;
            if (ok) { for (usize i = 0; i < n; ++i) if (v[i] & 1) ++*in; *pages += n; }
            mem_free(v);
            munmap(p, (size_t)st.st_size);
        }
    }
    close(fd);
    return ok;
#endif
}

u64 free_bytes(const char* dir) {
#if defined(_WIN32)
    ULARGE_INTEGER avail;
    return GetDiskFreeSpaceExA(dir, &avail, nullptr, nullptr) ? avail.QuadPart : 0;
#else
    struct statvfs v;
    return statvfs(dir, &v) == 0 ? (u64)v.f_bavail * (u64)v.f_frsize : 0;
#endif
}

// The file system's name, and whether it lives in memory (where nothing is cold).
void fs_name(const char* dir, char* out, usize cap, bool* in_memory) {
    *in_memory = false;
    snprintf(out, cap, "?");
#if defined(_WIN32)
    char root[MAX_PATH], name[64];
    if (GetVolumePathNameA(dir, root, sizeof root) && GetVolumeInformationA(root, nullptr, 0, nullptr, nullptr, nullptr, name, sizeof name)) snprintf(out, cap, "%s", name);
#elif defined(__linux__)
    struct statfs s;
    if (statfs(dir, &s) != 0) return;
    struct { u64 magic; const char* name; } known[] = {
        {0xEF53, "ext4"}, {0x9123683E, "btrfs"}, {0x58465342, "xfs"}, {0x01021994, "tmpfs"}, {0x2FC12FC1, "zfs"},
        {0x794c7630, "overlayfs"}, {0x6969, "nfs"}, {0xF2F52010, "f2fs"}, {0x65735546, "fuse"}, {0x858458f6, "ramfs"},
        {0x5346544e, "ntfs"}, {0x4d44, "vfat"}, {0x2011BAB0, "exfat"}, {0xCA451A4E, "bcachefs"},
    };
    snprintf(out, cap, "0x%llx", (unsigned long long)(u64)s.f_type);
    for (auto& k : known) if ((u64)(u32)s.f_type == k.magic) snprintf(out, cap, "%s", k.name);
    *in_memory = (u64)(u32)s.f_type == 0x01021994 || (u64)(u32)s.f_type == 0x858458f6;
#else
    struct statfs s;
    if (statfs(dir, &s) == 0) snprintf(out, cap, "%s", s.f_fstypename);
#endif
}

const char* platform_name() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__ANDROID__)
    return "android";
#elif defined(__linux__)
    return "linux";
#else
    return "?";
#endif
}

const char* config_name() {
#if KV_DEBUG
    return "debug";
#elif KV_DEV
    return "dev";
#else
    return "ship";
#endif
}

// CPU seconds of every child this process has waited for, and theirs (goldberg waits for
// its converters). Zero where the OS does not keep it that way (Windows).
f64 children_cpu() {
#if defined(_WIN32)
    return 0;
#else
    struct rusage u;
    if (getrusage(RUSAGE_CHILDREN, &u) != 0) return 0;
    return (f64)u.ru_utime.tv_sec + (f64)u.ru_utime.tv_usec * 1e-6 + (f64)u.ru_stime.tv_sec + (f64)u.ru_stime.tv_usec * 1e-6;
#endif
}

// ---- the plan ---------------------------------------------------------------------------------

struct Options {
    u64         size = 1ull << 30;
    u32         divisors[16] = {5, 10, 20, 100, 10000, 100000};
    u32         classes = 6;
    bool        hash = true;
    char        dir[700] = {};
    bool        keep = false, evict = true;
    u64         seed = 1;
    const char* watch = "auto";
    u32         touch = 1;
};

struct Class {
    u32  divisor;
    u64  base;          // a file's size before its jitter
    u32  count;
    u32  first;         // its first file in the list
    char name[32];      // "c3-51M"
};

struct File {
    u32 cls, index;
    u64 size;
    u64 seed;           // generation 0's
    u32 gen;            // 0, or 1 once touched
    // From the converter's record.
    bool timed;
    f64  t0, t1, t2, t3, cpu;
};

struct Bench {
    Options      o;
    char         exe_dir[700] = {};
    char         self[760] = {}, goldberg[760] = {};
    char         src[760] = {}, out[760] = {}, state[760] = {}, times[760] = {};
    Array<Class> classes{};
    Array<File>  files{};
    u64          total = 0;
    bool         failed = false;
};

void human(u64 bytes, char* out, usize cap) {
    const char* unit[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    f64 v = (f64)bytes;
    u32 u = 0;
    while (v >= 1024 && u < 4) { v /= 1024; ++u; }
    if (u == 0) snprintf(out, cap, "%llu B", (unsigned long long)bytes);
    else snprintf(out, cap, v < 10 ? "%.2f %s" : v < 100 ? "%.1f %s" : "%.0f %s", v, unit[u]);
}

void short_size(u64 bytes, char* out, usize cap) {
    const char* unit[] = {"", "K", "M", "G", "T"};
    u64 v = bytes;
    u32 u = 0;
    while (v >= 1024 && u < 4) { v /= 1024; ++u; }
    snprintf(out, cap, "%llu%s", (unsigned long long)v, unit[u]);
}

void source_rel(const Bench& b, const File& f, char* out, usize cap) {
    snprintf(out, cap, "%s/%03u/c%u-%06u.noise", b.classes[f.cls].name, f.index / 256, f.cls + 1, f.index);
}
void source_path(const Bench& b, const File& f, char* out, usize cap) {
    char rel[256];
    source_rel(b, f, rel, sizeof rel);
    snprintf(out, cap, "%s/%s", b.src, rel);
}
void output_rel(const Bench& b, const File& f, char* out, usize cap) {
    snprintf(out, cap, "bench/%s/%03u/c%u-%06u.cooked", b.classes[f.cls].name, f.index / 256, f.cls + 1, f.index);
}
void output_path(const Bench& b, const File& f, char* out, usize cap) {
    char rel[256];
    output_rel(b, f, rel, sizeof rel);
    snprintf(out, cap, "%s/%s", b.out, rel);
}
u64 file_seed(const Bench& b, const File& f) { return mix(b.o.seed ^ ((u64)(f.cls + 1) << 48) ^ ((u64)f.index << 8) ^ f.gen); }

void plan(Bench& b) {
    u32 n = b.o.classes;
    for (u32 k = 0; k < n; ++k) {
        Class c{};
        c.divisor = b.o.divisors[k];
        c.base = b.o.size / c.divisor;
        if (!c.base) c.base = 1;
        // Each class holds about the same share of the tree.
        f64 share = (f64)b.o.size / n;
        u32 count = (u32)(share / (f64)c.base + 0.5);
        c.count = count ? count : 1;
        c.first = b.files.len;
        char s[24];
        short_size(c.base, s, sizeof s);
        snprintf(c.name, sizeof c.name, "c%u-%s", k + 1, s);
        b.classes.push(c);
        for (u32 i = 0; i < c.count; ++i) {
            File f{};
            f.cls = k;
            f.index = i;
            // Sizes vary 20% either way within a class, the same on every machine.
            u64 r = mix(b.o.seed ^ 0x5157ull ^ ((u64)(k + 1) << 40) ^ i);
            f64 jitter = 0.8 + 0.4 * (f64)(r >> 11) * (1.0 / 9007199254740992.0);
            f.size = (u64)((f64)c.base * jitter);
            if (!f.size) f.size = 1;
            b.files.push(f);
            b.total += f.size;
        }
    }
    for (File& f : b.files) f.seed = file_seed(b, f);
}

// ---- writing the tree ------------------------------------------------------------------------

bool write_file(const char* path, u64 size, u64 seed) {
    char dir[800];
    snprintf(dir, sizeof dir, "%s", path);
    if (char* slash = strrchr(dir, '/')) { *slash = 0; io_make_dirs(dir); }
    FILE* fp = fopen(path, "wb");
    if (!fp) return false;
    const usize chunk = 1u << 20;
    u8* buf = (u8*)mem_alloc(chunk);
    bool ok = true;
    for (u64 at = 0; at < size && ok; at += chunk) {
        usize n = size - at < chunk ? (usize)(size - at) : chunk;
        fill(buf, n, seed, at);
        ok = fwrite(buf, 1, n, fp) == n;
    }
    mem_free(buf);
    return fclose(fp) == 0 && ok;
}

struct GenCtx { Bench* b; u32* failures; };

void gen_job(u32, u32 begin, u32 end, void* user) {
    GenCtx& g = *(GenCtx*)user;
    for (u32 i = begin; i < end; ++i) {
        File& f = g.b->files.data[i];
        char path[1200];
        source_path(*g.b, f, path, sizeof path);
        if (!write_file(path, f.size, f.seed)) atomic_fetch_add(g.failures, 1u, MO_RELAXED);
    }
}

void evict_job(u32, u32 begin, u32 end, void* user) {
    GenCtx& g = *(GenCtx*)user;
    for (u32 i = begin; i < end; ++i) {
        char path[1200];
        source_path(*g.b, g.b->files.data[i], path, sizeof path);
        if (!evict(path)) atomic_fetch_add(g.failures, 1u, MO_RELAXED);
    }
}

bool put_text(const char* path, const char* text) { return io_write_now(path, text, strlen(text)); }

bool write_rules(Bench& b) {
    char path[1200], text[3000];
    snprintf(path, sizeof path, "%s/goldberg.berg", b.src);
    bool ok = put_text(path, R"({
  "//": "goldberg_heavy's tree: noise in, every file cooked by xform.sh into one that lands",
  "types": { "noise": { "runtime": false }, "cooked": { "runtime": true } },
  "converters": { "xform": { "from": "noise", "to": "cooked", "shell": "xform", "outputs": ["{stem}.cooked"] } },
  "rules": [ { "match": "*.noise", "meta": { "type": "noise" } } ]
}
)");
    const char* mode = b.o.hash ? "hash" : "swap";
    snprintf(path, sizeof path, "%s/xform.sh", b.src);
    snprintf(text, sizeof text, "exec '%s' xform %s \"$1\" \"$2/$GOLDBERG_STEM.cooked\" '%s'\n", b.self, mode, b.times);
    ok = ok && put_text(path, text);
    snprintf(path, sizeof path, "%s/xform.bat", b.src);
    snprintf(text, sizeof text, "@\"%s\" xform %s \"%%~1\" \"%%~2/%%GOLDBERG_STEM%%.cooked\" \"%s\"\r\n", b.self, mode, b.times);
    return ok && put_text(path, text);
}

// ---- running goldberg -------------------------------------------------------------------------

struct Run {
    int  code = -1;
    f64  start = 0, seconds = 0, cpu = 0;
    char line[400] = {}; // goldberg's last line
};

bool count_one(void* user, const char*) { ++*(u32*)user; return true; }

// goldberg cook over the tree; while it runs, a line every five seconds with how many
// files the converter has finished (when `progress`).
Run cook(Bench& b, bool progress) {
    Run r;
    char root[800], st[800];
    snprintf(root, sizeof root, "bench=%s", b.src);
    snprintf(st, sizeof st, "%s/bench.json", b.state);
    const char* argv[] = {b.goldberg, "cook", "bench", "--root", root, "--out", b.out, "--state", st, "--project", b.o.dir, "--watch", b.o.watch};
    ProcessDesc d;
    d.argv = Span<const char* const>(argv, sizeof argv / sizeof argv[0]);
    Process p;
    Array<char> text{};
    f64 cpu0 = children_cpu();
    r.start = time_now();
    if (!p.spawn(d)) { fprintf(stderr, "goldberg_heavy: cannot run %s: %s\n", b.goldberg, p.error); return r; }
    bool out_open = true, err_open = true;
    char buf[4096];
    f64 next = r.start + 5.0;
    while (out_open || err_open || p.running()) {
        bool any = false;
        if (out_open) { i32 n = p.read_out(buf, sizeof buf); if (n > 0) { memcpy(text.push_n((u32)n), buf, (usize)n); any = true; } else if (n < 0) out_open = false; }
        if (err_open) { i32 n = p.read_err(buf, sizeof buf); if (n > 0) { fwrite(buf, 1, (usize)n, stderr); any = true; } else if (n < 0) err_open = false; }
        f64 now = time_now();
        if (progress && now >= next) {
            u32 done = 0;
            io_list_files(b.times, count_one, &done);
            printf("  ... %.0f s: %u of %u files converted\n", now - r.start, done, b.files.len);
            fflush(stdout);
            next = now + 5.0;
        }
        if (!any) time_sleep(0.002);
    }
    r.seconds = time_now() - r.start;
    r.code = p.exited ? p.code : -1;
    p.free();
    r.cpu = children_cpu() - cpu0;
    text.push(0);
    // The summary is goldberg's last line; the rest (notes) is passed on.
    char* last = text.data;
    for (char* q = text.data; *q; ++q) if (*q == '\n' && q[1]) last = q + 1;
    snprintf(r.line, sizeof r.line, "%s", last);
    if (char* nl = strchr(r.line, '\n')) *nl = 0;
    if (last != text.data) fwrite(text.data, 1, (usize)(last - text.data), stdout);
    text.free();
    return r;
}

// A watch started on the cooked tree, at its own share (40%): how long until it says it is
// stable, which it must do having cooked nothing; then one small source saved, and how
// long until its output in the tree is what it cooks to. The watch is killed after.
struct WatchRun { bool ok; f64 ready, react; char line[400]; };

bool output_is(const Bench& b, const File& f) {
    char path[1200];
    output_path(b, f, path, sizeof path);
    IoBytes got;
    if (!io_read_now(path, &got)) return false;
    bool same = got.size == f.size;
    if (same) {
        u8* src = (u8*)mem_alloc(f.size ? f.size : 1);
        u8* want = (u8*)mem_alloc(f.size ? f.size : 1);
        fill(src, f.size, f.seed, 0);
        transform(b.o.hash, src, want, f.size);
        same = memcmp(got.data, want, f.size) == 0;
        mem_free(src);
        mem_free(want);
    }
    got.free();
    return same;
}

WatchRun watch(Bench& b, File& saved) {
    WatchRun r{};
    char root[800], st[800];
    snprintf(root, sizeof root, "bench=%s", b.src);
    snprintf(st, sizeof st, "%s/bench.json", b.state);
    const char* argv[] = {b.goldberg, "watch", "bench", "--root", root, "--out", b.out, "--state", st, "--project", b.o.dir, "--watch", b.o.watch};
    ProcessDesc d;
    d.argv = Span<const char* const>(argv, sizeof argv / sizeof argv[0]);
    Process p;
    Array<char> text{};
    f64 start = time_now();
    if (!p.spawn(d)) { fprintf(stderr, "goldberg_heavy: cannot run %s: %s\n", b.goldberg, p.error); return r; }
    char buf[4096];
    auto read_all = [&] {
        i32 n;
        while ((n = p.read_out(buf, sizeof buf)) > 0) memcpy(text.push_n((u32)n), buf, (usize)n);
        while ((n = p.read_err(buf, sizeof buf)) > 0) fwrite(buf, 1, (usize)n, stderr);
    };
    for (;;) {
        read_all();
        text.push(0);
        const char* at = strstr(text.data, ", watching with ");
        --text.len;
        if (at) {
            const char* begin = at;
            while (begin > text.data && begin[-1] != '\n') --begin;
            const char* end = strchr(at, '\n');
            snprintf(r.line, sizeof r.line, "%.*s", (int)(end ? end - begin : (long)strlen(begin)), begin);
            r.ready = time_now() - start;
            break;
        }
        if (!p.running() || time_now() - start > 600) { p.free(); text.free(); return r; }
        time_sleep(0.002);
    }
    // One small source saved with new bytes.
    saved.gen = 2;
    saved.seed = file_seed(b, saved);
    char path[1200];
    source_path(b, saved, path, sizeof path);
    f64 t0 = time_now();
    bool wrote = write_file(path, saved.size, saved.seed);
    while (wrote && !output_is(b, saved) && time_now() - t0 < 60) { read_all(); time_sleep(0.001); }
    r.react = time_now() - t0;
    r.ok = wrote && output_is(b, saved);
    p.kill();
    p.free();
    text.free();
    return r;
}

// Reads the converter's record of each file it ran, and removes it, so the next cook's
// records are its own. The number read.
u32 read_times(Bench& b, bool only_touched) {
    u32 n = 0;
    for (File& f : b.files) {
        if (only_touched && !f.gen) continue;
        char path[1200];
        snprintf(path, sizeof path, "%s/c%u-%06u", b.times, f.cls + 1, f.index);
        IoBytes bytes;
        f.timed = false;
        if (!io_read_now(path, &bytes)) continue;
        unsigned long long size = 0;
        if (sscanf((const char*)bytes.data, "%llu %lf %lf %lf %lf %lf", &size, &f.t0, &f.t1, &f.t2, &f.t3, &f.cpu) == 6) { f.timed = true; ++n; }
        bytes.free();
        io_remove(path);
    }
    return n;
}

// ---- checking what it made ---------------------------------------------------------------

struct VerifyCtx {
    Bench*    b;
    u8*       digests;  // 32 bytes a file
    u32       missing, wrong_size, wrong_bytes;
    const u8* deep;     // per file: compare every byte with what it should be
};

// Every output: there, the size of its source, and its SHA-256 for the tree's digest. The
// ones marked deep are made again here and compared byte for byte.
void verify_job(u32, u32 begin, u32 end, void* user) {
    VerifyCtx& v = *(VerifyCtx*)user;
    const usize chunk = 1u << 20;
    u8* buf = (u8*)mem_alloc(chunk);
    u8* src = (u8*)mem_alloc(chunk + 32);
    u8* want = (u8*)mem_alloc(chunk + 32);
    for (u32 i = begin; i < end; ++i) {
        File& f = v.b->files.data[i];
        char path[1200];
        output_path(*v.b, f, path, sizeof path);
        FILE* fp = fopen(path, "rb");
        if (!fp) { atomic_fetch_add(&v.missing, 1u, MO_RELAXED); continue; }
        Sha256 h;
        h.init();
        u64 got = 0;
        bool same = true;
        usize n;
        while ((n = fread(buf, 1, chunk, fp)) > 0) {
            if (v.deep[i] && same) {
                // chunk is a multiple of 32: the blocks line up with the converter's.
                usize len = got + n > f.size ? (got < f.size ? (usize)(f.size - got) : 0) : n;
                fill(src, len, f.seed, got);
                transform(v.b->o.hash, src, want, len);
                same = len == n && memcmp(buf, want, n) == 0;
            }
            h.update(buf, n);
            got += n;
        }
        fclose(fp);
        h.finish(v.digests + 32 * (usize)i);
        if (got != f.size) atomic_fetch_add(&v.wrong_size, 1u, MO_RELAXED);
        else if (v.deep[i] && !same) atomic_fetch_add(&v.wrong_bytes, 1u, MO_RELAXED);
    }
    mem_free(buf);
    mem_free(src);
    mem_free(want);
}

struct Verified { bool ok; f64 seconds; char digest[65]; u32 deep, extra; };

bool count_out(void* user, const char* rel) {
    const char* dot = strrchr(rel, '.');
    if (!dot || strcmp(dot, ".cooked") != 0) ++*(u32*)user;
    return true;
}

Verified verify(Bench& b, bool touched_only_deep) {
    Verified r{};
    f64 t0 = time_now();
    VerifyCtx v{};
    v.b = &b;
    v.digests = (u8*)mem_alloc(32 * (usize)b.files.len);
    u8* deep = (u8*)mem_alloc(b.files.len);
    memset(deep, 0, b.files.len);
    // First, middle and last of each class, and whatever was touched.
    for (const Class& c : b.classes) {
        if (touched_only_deep) break;
        deep[c.first] = deep[c.first + c.count / 2] = deep[c.first + c.count - 1] = 1;
    }
    for (u32 i = 0; i < b.files.len; ++i) if (b.files[i].gen) deep[i] = 1;
    for (u32 i = 0; i < b.files.len; ++i) r.deep += deep[i];
    v.deep = deep;
    jobs_run(b.files.len, 4, verify_job, &v, "heavy.verify");
    // The tree's digest: each output's path and digest, in the plan's order.
    Sha256 h;
    h.init();
    for (u32 i = 0; i < b.files.len; ++i) {
        char rel[256];
        output_rel(b, b.files[i], rel, sizeof rel);
        h.update(rel, strlen(rel) + 1);
        h.update(v.digests + 32 * (usize)i, 32);
    }
    u8 d[32];
    h.finish(d);
    sha256_hex(d, r.digest);
    // Nothing else in the tree (its own marks are hidden, and not counted).
    io_list_files(b.out, count_out, &r.extra);
    r.ok = !v.missing && !v.wrong_size && !v.wrong_bytes && !r.extra;
    if (v.missing) fprintf(stderr, "goldberg_heavy: %u outputs missing\n", v.missing);
    if (v.wrong_size) fprintf(stderr, "goldberg_heavy: %u outputs the wrong size\n", v.wrong_size);
    if (v.wrong_bytes) fprintf(stderr, "goldberg_heavy: %u outputs with the wrong bytes\n", v.wrong_bytes);
    if (r.extra) fprintf(stderr, "goldberg_heavy: %u files in the output tree that no source accounts for\n", r.extra);
    mem_free(v.digests);
    mem_free(deep);
    r.seconds = time_now() - t0;
    return r;
}

// ---- the report -------------------------------------------------------------------------------

int cmp_f64(const void* a, const void* b) {
    f64 x = *(const f64*)a, y = *(const f64*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

f64 pick(const f64* sorted, u32 n, f64 q) { return n ? sorted[(u32)((n - 1) * q + 0.5)] : 0; }

void ms(f64 s, char* out, usize cap) {
    if (s < 0.001) snprintf(out, cap, "%.0f us", s * 1e6);
    else if (s < 1) snprintf(out, cap, "%.1f ms", s * 1e3);
    else snprintf(out, cap, "%.2f s", s);
}

void report_classes(Bench& b, f64 cook_start) {
    printf("\n  %-9s %7s %9s %9s  %9s %9s %9s %9s  %8s  %8s %8s  %s\n", "class", "files", "each", "total",
           "mean", "p50", "p95", "max", "MB/s", "read", "write", "window");
    f64* steps = (f64*)mem_alloc(sizeof(f64) * (b.files.len + 1));
    for (const Class& c : b.classes) {
        u32 n = 0;
        u64 bytes = 0;
        f64 sum = 0, rd = 0, wr = 0, first = 1e30, last = 0;
        for (u32 i = c.first; i < c.first + c.count; ++i) {
            const File& f = b.files[i];
            bytes += f.size;
            if (!f.timed) continue;
            steps[n++] = f.t3 - f.t0;
            sum += f.t3 - f.t0;
            rd += f.t1 - f.t0;
            wr += f.t3 - f.t2;
            if (f.t0 < first) first = f.t0;
            if (f.t3 > last) last = f.t3;
        }
        qsort(steps, n, sizeof(f64), cmp_f64);
        char each[24], total[16], a[16], p50[16], p95[16], mx[16], r[16], w[16];
        short_size(c.base, each, sizeof each);
        human(bytes, total, sizeof total);
        ms(n ? sum / n : 0, a, sizeof a);
        ms(pick(steps, n, 0.5), p50, sizeof p50);
        ms(pick(steps, n, 0.95), p95, sizeof p95);
        ms(n ? steps[n - 1] : 0, mx, sizeof mx);
        snprintf(r, sizeof r, "%.0f%%", sum > 0 ? 100 * rd / sum : 0);
        snprintf(w, sizeof w, "%.0f%%", sum > 0 ? 100 * wr / sum : 0);
        char div[16];
        snprintf(div, sizeof div, "1/%u", c.divisor);
        printf("  %-9s %7u %9s %9s  %9s %9s %9s %9s  %8.0f  %8s %8s  ", div, c.count, each, total, a, p50, p95, mx,
               sum > 0 ? (f64)bytes / 1e6 / sum : 0, r, w);
        if (n) printf("%.2f s to %.2f s\n", first - cook_start, last - cook_start);
        else printf("-\n");
        if (n != c.count) printf("  %-9s %u of %u runs left no record\n", "", c.count - n, c.count);
    }
    mem_free(steps);
    printf("\n  mean/p50/p95/max: one converter run, from its start to its output written. MB/s: one run's\n"
           "  rate, the class's bytes over the sum of its runs. read/write: their share of a run. window: when\n"
           "  the class's first run started and its last ended, from the start of the cook.\n");
}

// ---- the bench --------------------------------------------------------------------------------

bool parse_size(const char* s, u64* out) {
    char* end;
    f64 v = strtod(s, &end);
    if (end == s || v <= 0) return false;
    u64 mul = 1;
    switch (*end) {
    case 'k': case 'K': mul = 1ull << 10; ++end; break;
    case 'm': case 'M': mul = 1ull << 20; ++end; break;
    case 'g': case 'G': mul = 1ull << 30; ++end; break;
    case 't': case 'T': mul = 1ull << 40; ++end; break;
    default: break;
    }
    if (*end == 'i' || *end == 'B' || *end == 'b') ++end;
    if (*end == 'B') ++end;
    if (*end) return false;
    *out = (u64)(v * (f64)mul);
    return true;
}

int usage(const char* why) {
    if (why) fprintf(stderr, "goldberg_heavy: %s\n", why);
    fprintf(stderr,
            "usage: goldberg_heavy [--size 1G] [--classes 5,10,20,100,10000,100000] [--work hash|swap]\n"
            "                      [--dir DIR] [--seed N] [--touch N] [--watch auto|inotify|poll|none]\n"
            "                      [--no-evict] [--keep]\n"
            "  --size      about how big the tree is (K, M, G)\n"
            "  --classes   the size classes, each a file of 1/N of --size; each class holds about the\n"
            "              same share of the tree\n"
            "  --work      what the converter does: hash (every 32 bytes become their SHA-256) or swap\n"
            "              (every pair of bytes swapped)\n"
            "  --dir       where the tree goes (default build/<target>/bench/goldberg); it needs about\n"
            "              three times --size free, and is removed after unless --keep\n"
            "  --touch     files changed per class before the last cook (default 1)\n"
            "  --watch     goldberg's --watch (default auto, as under kormake)\n"
            "  --no-evict  leave the OS's cache warm\n");
    return 2;
}

bool parse(Options& o, int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (!strcmp(a, "--keep")) o.keep = true;
        else if (!strcmp(a, "--no-evict")) o.evict = false;
        else if (!v) { usage(a[0] == '-' ? "an option without its value" : "an unknown word"); return false; }
        else if (!strcmp(a, "--size")) { if (!parse_size(v, &o.size) || o.size < (1u << 20)) { usage("--size: a size of at least 1M, like 512M or 2G"); return false; } ++i; }
        else if (!strcmp(a, "--work")) {
            if (!strcmp(v, "hash")) o.hash = true;
            else if (!strcmp(v, "swap")) o.hash = false;
            else { usage("--work is hash or swap"); return false; }
            ++i;
        }
        else if (!strcmp(a, "--classes")) {
            o.classes = 0;
            for (const char* p = v; *p;) {
                char* end;
                unsigned long d = strtoul(p, &end, 10);
                if (end == p || d < 1 || o.classes == 16) { usage("--classes: up to 16 divisors, like 5,10,100"); return false; }
                o.divisors[o.classes++] = (u32)d;
                p = *end == ',' ? end + 1 : end;
                if (*end && *end != ',') { usage("--classes: up to 16 divisors, like 5,10,100"); return false; }
            }
            ++i;
        }
        else if (!strcmp(a, "--dir")) { snprintf(o.dir, sizeof o.dir, "%s", v); ++i; }
        else if (!strcmp(a, "--seed")) { o.seed = strtoull(v, nullptr, 10); ++i; }
        else if (!strcmp(a, "--touch")) { o.touch = (u32)strtoul(v, nullptr, 10); ++i; }
        else if (!strcmp(a, "--watch")) { o.watch = v; ++i; }
        else { usage("an unknown option"); return false; }
    }
    return true;
}

const char* kMark = ".goldberg-heavy"; // in --dir: the tree is this bench's, to remove

// Whether goldberg's line says it cooked something, and how many.
bool cooked(const char* line, u32* n) {
    const char* p = strstr(line, " files, ");
    return p && sscanf(p, " files, %u cooked", n) == 1;
}

bool step(Bench& b, bool ok, const char* what) {
    if (!ok) { fprintf(stderr, "goldberg_heavy: FAIL %s\n", what); b.failed = true; }
    return ok;
}

int bench(Options& o) {
    Bench b;
    b.o = o;
    if (!process_supported()) { printf("goldberg_heavy: this platform cannot start processes yet: nothing to measure\n"); return 0; }
    if (!platform_exe_dir(b.exe_dir, sizeof b.exe_dir)) { fprintf(stderr, "goldberg_heavy: cannot find my own directory\n"); return 1; }
    snprintf(b.self, sizeof b.self, "%sgoldberg_heavy%s", b.exe_dir, kExe);
    snprintf(b.goldberg, sizeof b.goldberg, "%sgoldberg%s", b.exe_dir, kExe);
    if (!io_is_file(b.goldberg)) { fprintf(stderr, "goldberg_heavy: no goldberg beside me (%s): build it\n", b.goldberg); return 1; }
    if (!b.o.dir[0]) {
        // build/<target>/bin/ -> build/<target>/bench/goldberg. Never the OS's temporary
        // directory, which is memory on some systems.
        snprintf(b.o.dir, sizeof b.o.dir, "%s", b.exe_dir);
        usize n = strlen(b.o.dir);
        if (n >= 4 && !strcmp(b.o.dir + n - 4, "bin/")) b.o.dir[n - 4] = 0;
        usize m = strlen(b.o.dir);
        snprintf(b.o.dir + m, sizeof b.o.dir - m, "bench/goldberg");
    }
    if (strchr(b.o.dir, '\'') || strchr(b.self, '\'')) { fprintf(stderr, "goldberg_heavy: a path with a ' in it: use --dir\n"); return 2; }
    char mark[800];
    snprintf(mark, sizeof mark, "%s/%s", b.o.dir, kMark);
    if (io_is_dir(b.o.dir)) {
        if (!io_is_file(mark)) { fprintf(stderr, "goldberg_heavy: %s is there and is not a tree I made: give --dir a new one\n", b.o.dir); return 2; }
        io_remove_tree(b.o.dir);
    }
    snprintf(b.src, sizeof b.src, "%s/src", b.o.dir);
    snprintf(b.out, sizeof b.out, "%s/out", b.o.dir);
    snprintf(b.state, sizeof b.state, "%s/state", b.o.dir);
    snprintf(b.times, sizeof b.times, "%s/times", b.o.dir);
    if (!io_make_dirs(b.src) || !io_make_dirs(b.state) || !io_make_dirs(b.times) || !put_text(mark, "goldberg_heavy's scratch: removed at its next run\n")) {
        fprintf(stderr, "goldberg_heavy: cannot make %s\n", b.o.dir);
        return 1;
    }
    plan(b);
    // Sources, what the converter makes (kept in the state's work tree) and the output
    // tree: three copies.
    u64 need = b.total * 3 + (64u << 20), have = free_bytes(b.o.dir);
    char s1[32], s2[32];
    if (have && have < need) {
        human(need, s1, sizeof s1);
        human(have, s2, sizeof s2);
        fprintf(stderr, "goldberg_heavy: this needs %s free at %s and there is %s: a smaller --size, or --dir elsewhere\n", s1, b.o.dir, s2);
        io_remove_tree(b.o.dir);
        return 2;
    }
    char fs[64];
    bool in_memory;
    fs_name(b.o.dir, fs, sizeof fs, &in_memory);

    human(b.total, s1, sizeof s1);
    printf("goldberg heavy: a synthetic cook\n");
    printf("  machine  %s, %u cpus, %s build\n", platform_name(), os_cpu_count(), config_name());
    printf("  disk     %s, %s\n", fs, b.o.dir);
    printf("  tree     %s in %u files, %u classes, seed %llu\n", s1, b.files.len, b.classes.len, (unsigned long long)b.o.seed);
    printf("  work     %s\n", b.o.hash ? "hash: every 32 bytes become their SHA-256" : "swap: every pair of bytes swapped");
    if (in_memory) printf("  note     %s is in memory: nothing here is ever cold\n", fs);
    fflush(stdout);

    // The tree.
    f64 t = time_now();
    u32 failures = 0;
    GenCtx g{&b, &failures};
    jobs_run(b.files.len, 1, gen_job, &g, "heavy.generate");
    bool rules = write_rules(b);
    f64 gen_s = time_now() - t;
    if (!step(b, failures == 0 && rules, "writing the tree")) { io_remove_tree(b.o.dir); return 1; }
    printf("\n  %-26s %9.2f s   %.0f MB/s\n", "generate", gen_s, (f64)b.total / 1e6 / gen_s);

    // Out of the cache.
    if (b.o.evict) {
        t = time_now();
        failures = 0;
        jobs_run(b.files.len, 16, evict_job, &g, "heavy.evict");
        f64 ev_s = time_now() - t;
        u64 in = 0, pages = 0;
        bool told = true;
        for (const File& f : b.files) {
            char path[1200];
            source_path(b, f, path, sizeof path);
            if (!resident(path, &in, &pages)) { told = false; break; }
        }
        char note[80];
        if (told && pages) snprintf(note, sizeof note, "%.1f%% still in memory", 100.0 * (f64)in / (f64)pages);
        else snprintf(note, sizeof note, "what stays in memory not measured here");
        printf("  %-26s %9.2f s   %s%s\n", "evict from the cache", ev_s, note, failures ? " (some files refused)" : "");
    } else {
        printf("  %-26s %11s   --no-evict\n", "evict from the cache", "-");
    }
    fflush(stdout);

    // The cook.
    printf("  cold cook ...\n");
    fflush(stdout);
    Run cold = cook(b, true);
    u32 ran = read_times(b, false);
    printf("  %-26s %9.2f s   %.0f MB/s, %.0f files/s\n", "cold cook", cold.seconds, (f64)b.total / 1e6 / cold.seconds, b.files.len / cold.seconds);
    printf("    %s\n", cold.line);
    bool ok = step(b, cold.code == 0, "the cold cook exited non-zero") & step(b, ran == b.files.len, "a converter run left no record");
    f64 first = 1e30, last = 0, busy = 0, conv_cpu = 0;
    for (const File& f : b.files) {
        if (!f.timed) continue;
        if (f.t0 < first) first = f.t0;
        if (f.t3 > last) last = f.t3;
        busy += f.t3 - f.t0;
        conv_cpu += f.cpu;
    }
    if (ran) {
        f64 window = last - first;
        printf("    %-24s %9.2f s   start, scan, hash the sources, solve\n", "before the first run", first - cold.start);
        printf("    %-24s %9.2f s   %u runs; %.0f%% of %u at once\n", "converters running", window, ran, window > 0 ? 100 * busy / (window * kSteps) : 0, kSteps);
        printf("    %-24s %9.2f s   the last outputs landed, the state written\n", "after the last run", cold.start + cold.seconds - last);
        if (cold.cpu > 0)
            printf("    %-24s %9.2f s   converters %.2f s, goldberg and the shell %.2f s\n", "cpu", cold.cpu, conv_cpu, cold.cpu - conv_cpu);
    }
    report_classes(b, cold.start);
    printf("\n");
    fflush(stdout);
    Verified v = verify(b, false);
    ok = step(b, v.ok, "the output tree is not what the sources cook to") && ok;
    printf("  %-26s %9.2f s   %u outputs, %u made again and compared\n", "verify", v.seconds, b.files.len, v.deep);

    // Again, with nothing changed.
    Run noop = cook(b, false);
    u32 n = 99;
    ok = step(b, noop.code == 0 && cooked(noop.line, &n) && n == 0, "a second cook of an unchanged tree cooked something") && ok;
    printf("  %-26s %9.2f s   %s\n", "cook, nothing changed", noop.seconds, noop.line);

    // Again, with a few files in each class changed.
    u32 touched = 0;
    for (const Class& c : b.classes) {
        u32 k = b.o.touch < c.count ? b.o.touch : c.count;
        for (u32 j = 0; j < k; ++j) {
            File& f = b.files[c.first + (u32)(((u64)j * c.count + c.count / 2) / k) % c.count];
            if (f.gen) continue;
            f.gen = 1;
            f.seed = file_seed(b, f);
            char path[1200];
            source_path(b, f, path, sizeof path);
            ok = step(b, write_file(path, f.size, f.seed), "rewriting a file") && ok;
            if (b.o.evict) evict(path);
            ++touched;
        }
    }
    if (touched) {
        Run again = cook(b, false);
        u32 re = read_times(b, true);
        ok = step(b, again.code == 0 && re == touched, "the cook after the change did not run the converter on each changed file") && ok;
        char label[48];
        snprintf(label, sizeof label, "cook, %u changed", touched);
        printf("  %-26s %9.2f s   %s\n", label, again.seconds, again.line);
        Verified v2 = verify(b, true);
        ok = step(b, v2.ok, "the output tree after the change is not what the sources cook to") && ok;
        printf("  %-26s %9.2f s   %u made again and compared\n", "verify", v2.seconds, v2.deep);
    }

    // A watch on what is there now: ready, and one change.
    {
        File& saved = b.files[b.classes[b.classes.len - 1].first]; // the smallest class's first
        WatchRun wr = watch(b, saved);
        u32 n = 99;
        ok = step(b, wr.ready > 0 && cooked(wr.line, &n) && n == 0, "a watch started on the cooked tree cooked something, or never said it was stable") && ok;
        ok = step(b, wr.ok, "a watch did not cook a saved source") && ok;
        printf("  %-26s %9.2f s   %s\n", "watch, until stable", wr.ready, wr.line);
        char each[16];
        short_size(saved.size, each, sizeof each);
        printf("  %-26s %9.1f ms  a %s source saved, until its output is right\n", "watch, one save", wr.react * 1000, each);
    }

    // The first cook's: the same on every machine for the same --size, --classes, --seed
    // and --work.
    printf("\n  digest   %s\n", v.digest);
    if (!b.o.keep) io_remove_tree(b.o.dir);
    else printf("  kept     %s\n", b.o.dir);
    printf("goldberg_heavy: %s: cold %.2f s, nothing changed %.2f s, digest %.16s\n", ok && !b.failed ? "ok" : "FAILED", cold.seconds, noop.seconds, v.digest);
    b.classes.free();
    b.files.free();
    return ok && !b.failed ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "xform")) {
        int rc = xform(argc, argv);
        mem_shutdown();
        return rc;
    }
    Options o;
    if (!parse(o, argc, argv)) return 2;
    jobs_init(os_cpu_count() > 1 ? os_cpu_count() - 1 : 0);
    int rc = bench(o);
    jobs_shutdown();
    names_shutdown();
    mem_shutdown();
    return rc;
}
