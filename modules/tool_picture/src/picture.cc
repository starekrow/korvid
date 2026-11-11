// picture: commands start a job, the system runs it. A job is a list of pictures (A's,
// then B's), read through the I/O queue and decoded to RGB8, then compared pair by pair or
// encoded, and what that makes (difference pictures, PNGs) written back through the queue.
//
// The comparison: for each pair the size (a different size fails), the largest difference
// in any channel, the pixels whose largest channel difference passes the threshold and
// their fraction of the picture, and the first such pixel with both values. A pair passes
// when that fraction is at most --max-fraction and no channel differs by more than
// --max-channel. Two GPUs round linear filtering and blending differently and an edge may
// land a pixel over, so a few pixels off by a little are expected; a wrong blend, a
// missing layer, a sort out of order or a picture upside down changes many pixels by a
// lot. The defaults are a first guess, to be fixed by the first comparison of two backends
// and not loosened after (docs/PRDs/Engine-Slice-7-PRD.md, R9).
#include "korvid/tool_picture/module.h"
#include "korvid/core/array.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/sort.h"
#include "korvid/image/png.h"
#include "korvid/image/ppm.h"
#include "korvid/platform/io.h"
#include "korvid/tool/tool.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace kv {

namespace {

enum class Op : u8 { None, Compare, Png };
enum class Stage : u8 { Idle, Read, Write };

struct Pic {
    char   name[128];  // the stem: what pairs A's with B's, and names what is written
    char   path[1100];
    Handle io;
    u32    width = 0, height = 0;
    u8*    rgb   = nullptr; // width * height * 3, from the module's allocator
    bool   done = false, ok = false;
};

struct PictureState {
    Allocator* general = nullptr;
    Op         op      = Op::None;
    Stage      stage   = Stage::Idle;
    char       a[512] = {}, b[512] = {}, out[512] = {}; // out: --diff's directory, or png's
    u32        threshold = 8, max_channel = 255;
    f32        max_fraction = 0.005f;
    Array<Pic> pics;   // A's (count_a of them), then B's
    Array<i32> match;  // for each of A's: the index of its B in pics, or -1
    u32        count_a = 0;
    u32        submitted = 0, completed = 0, failures = 0, writes_out = 0, write_failures = 0;
};

PictureState* state(Engine& e) { return engine_module_state_as<PictureState>(e, "tool_picture"); }

void say(const char* fmt, ...) KV_PRINTF(1, 2);
void say(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    fputc('\n', stdout);
}

void free_pic(PictureState& s, Pic& p) {
    if (p.rgb) free_in(s.general, p.rgb, (usize)p.width * p.height * 3);
    p.rgb = nullptr;
}

void reset(PictureState& s) {
    for (Pic& p : s.pics) free_pic(s, p);
    s.pics.clear();
    s.match.clear();
    s.op = Op::None; s.stage = Stage::Idle;
    s.count_a = s.submitted = s.completed = s.failures = s.writes_out = s.write_failures = 0;
    s.out[0] = 0;
}

void finish(Engine& e, PictureState& s, int code) {
    reset(s);
    tool_finish(e, code);
}

bool busy(CliContext& ctx, PictureState* s) {
    if (!s) { ctx.fail("the tool_picture module is not up"); return true; }
    if (s->op != Op::None) { ctx.fail("picture is busy with another job"); return true; }
    return false;
}

bool ends_with(const char* s, const char* tail) {
    usize n = strlen(s), m = strlen(tail);
    if (m > n) return false;
    for (usize i = 0; i < m; ++i) {
        char a = s[n - m + i], b = tail[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool is_picture(const char* name) { return ends_with(name, ".ppm") || ends_with(name, ".png"); }

// The file name without its directory or extension.
void stem_of(const char* path, char* out, usize cap) {
    const char* base = path;
    for (const char* p = path; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    usize n = strlen(base);
    const char* dot = strrchr(base, '.');
    if (dot && dot != base) n = (usize)(dot - base);
    if (n >= cap) n = cap - 1;
    memcpy(out, base, n);
    out[n] = 0;
}

bool add_pic(PictureState& s, const char* dir, const char* name) {
    Pic p{};
    int n = dir ? snprintf(p.path, sizeof p.path, "%s/%s", dir, name) : snprintf(p.path, sizeof p.path, "%s", name);
    if (n < 0 || (usize)n >= sizeof p.path) { KV_ERROR("picture", "path too long: '%s'", name); return false; }
    stem_of(name, p.name, sizeof p.name);
    s.pics.push(p);
    return true;
}

// A directory's pictures, its own files only (not its subdirectories'), in name order.
struct Listing { PictureState* s; const char* dir; bool ok; };
bool add_listed(void* user, const char* relative) {
    Listing& l = *(Listing*)user;
    if (strchr(relative, '/') || !is_picture(relative)) return true;
    l.ok = add_pic(*l.s, l.dir, relative) && l.ok;
    return true;
}

int by_name(const void* a, const void* b) { return strcmp(((const Pic*)a)->name, ((const Pic*)b)->name); }

// Adds `path`'s pictures: every picture in it if a directory, else the file. False with
// the reason given if it is neither.
bool gather(CliContext& ctx, PictureState& s, const char* path, bool* is_dir) {
    u32 from = s.pics.len;
    Listing l{&s, path, true};
    i32 n = io_list_files(path, add_listed, &l);
    if (!l.ok) { ctx.fail("cannot list '%s'", path); return false; }
    *is_dir = n >= 0;
    if (n < 0) {
        if (!is_picture(path)) { ctx.fail("'%s' is not a directory, a .ppm or a .png", path); return false; }
        if (!add_pic(s, nullptr, path)) { ctx.fail("path too long: '%s'", path); return false; }
    }
    if (s.pics.len - from > 1) sort(s.pics.data + from, s.pics.len - from, sizeof(Pic), by_name);
    return true;
}

// ---- commands ----------------------------------------------------------------------------------

// picture compare <a> <b> [--threshold N] [--max-fraction F] [--max-channel N] [--diff DIR]
void cmd_compare(CliContext& ctx, Span<const char* const> args) {
    PictureState* s = state(*ctx.engine);
    if (busy(ctx, s)) return;
    const char* usage = "picture compare <a> <b> [--threshold N] [--max-fraction F] [--max-channel N] [--diff DIR]";
    const char* pos[2] = {};
    u32 npos = 0;
    s->threshold = 8; s->max_fraction = 0.005f; s->max_channel = 255; s->out[0] = 0;
    for (u32 i = 0; i < args.len; ++i) {
        const char* a = args[i];
        bool has_value = i + 1 < args.len;
        if (!strcmp(a, "--threshold") && has_value) { if (!cli_arg_u32(ctx, args[++i], "--threshold", &s->threshold)) return; }
        else if (!strcmp(a, "--max-channel") && has_value) { if (!cli_arg_u32(ctx, args[++i], "--max-channel", &s->max_channel)) return; }
        else if (!strcmp(a, "--max-fraction") && has_value) { if (!cli_arg_f32(ctx, args[++i], "--max-fraction", &s->max_fraction)) return; }
        else if (!strcmp(a, "--diff") && has_value) snprintf(s->out, sizeof s->out, "%s", args[++i]);
        else if (a[0] == '-' && a[1] == '-') { ctx.fail("%s: unknown option '%s'", usage, a); return; }
        else if (npos < 2) pos[npos++] = a;
        else { ctx.fail("%s", usage); return; }
    }
    if (npos != 2) { ctx.fail("%s", usage); return; }
    snprintf(s->a, sizeof s->a, "%s", pos[0]);
    snprintf(s->b, sizeof s->b, "%s", pos[1]);
    bool a_dir = false, b_dir = false;
    if (!gather(ctx, *s, s->a, &a_dir)) { reset(*s); return; }
    s->count_a = s->pics.len;
    if (s->count_a == 0) { ctx.fail("no .ppm or .png pictures in '%s'", s->a); reset(*s); return; }
    if (!gather(ctx, *s, s->b, &b_dir)) { reset(*s); return; }
    // Each of A's pictures meets B's of the same stem (.ppm before .png), or B itself when
    // B is one file.
    for (u32 i = 0; i < s->count_a; ++i) {
        i32 m = -1;
        if (!b_dir) m = (i32)s->count_a;
        else
            for (u32 j = s->count_a; j < s->pics.len; ++j)
                if (!strcmp(s->pics[j].name, s->pics[i].name) && (m < 0 || ends_with(s->pics[j].path, ".ppm"))) m = (i32)j;
        s->match.push(m);
    }
    s->op = Op::Compare;
    s->stage = Stage::Read;
}

// picture png <a> <dir>: every picture of A as a PNG in dir
void cmd_png(CliContext& ctx, Span<const char* const> args) {
    PictureState* s = state(*ctx.engine);
    if (busy(ctx, s)) return;
    if (args.len != 2) { ctx.fail("picture png <file or dir> <out dir>: every picture as a PNG, to keep as a reference"); return; }
    snprintf(s->a, sizeof s->a, "%s", args[0]);
    snprintf(s->out, sizeof s->out, "%s", args[1]);
    bool dir = false;
    if (!gather(ctx, *s, s->a, &dir)) { reset(*s); return; }
    s->count_a = s->pics.len;
    if (s->count_a == 0) { ctx.fail("no .ppm or .png pictures in '%s'", s->a); reset(*s); return; }
    s->op = Op::Png;
    s->stage = Stage::Read;
}

// ---- the job -----------------------------------------------------------------------------------

// A read file, decoded to RGB8 (a PNG's alpha dropped).
bool decode(PictureState& s, Pic& p, const u8* data, usize size) {
    const char* why = nullptr;
    if (ends_with(p.path, ".png")) {
        PngImage img;
        if (!png_decode(data, size, s.general, img, &why)) { KV_ERROR("picture", "%s: %s", p.path, why); return false; }
        usize n = (usize)img.width * img.height;
        u8* rgb = (u8*)alloc_in(s.general, n * 3);
        if (rgb) for (usize i = 0; i < n; ++i) { rgb[i * 3] = img.rgba[i * 4]; rgb[i * 3 + 1] = img.rgba[i * 4 + 1]; rgb[i * 3 + 2] = img.rgba[i * 4 + 2]; }
        free_in(s.general, img.rgba, n * 4);
        if (!rgb) { KV_ERROR("picture", "%s: out of memory", p.path); return false; }
        p.width = img.width; p.height = img.height; p.rgb = rgb;
        return true;
    }
    PpmImage img;
    if (!ppm_decode(data, size, s.general, img, &why)) { KV_ERROR("picture", "%s: %s", p.path, why); return false; }
    p.width = img.width; p.height = img.height; p.rgb = img.rgb;
    return true;
}

// Encodes `rgb` as a PNG and submits its write to dir/name.
void write_png(Engine& e, PictureState& s, const char* name, const u8* rgb, u32 w, u32 h) {
    Array<u8> file;
    file.alloc = s.general;
    const char* why = nullptr;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", s.out, name);
    if (!png_encode(rgb, w, h, 3, file, &why)) { KV_ERROR("picture", "%s: %s", path, why); ++s.write_failures; return; }
    IoWriteRequest r;
    r.path = path; r.data = file.data; r.size = file.len; r.alloc = s.general; r.user = 1;
    if (platform_io(e)->write_file(r).is_null()) { KV_ERROR("picture", "cannot write '%s'", path); ++s.write_failures; }
    else ++s.writes_out;
    file.free(); // the write took a copy
}

// One pair: its line, and its difference picture when asked and there is any. True if it passed.
bool compare(Engine& e, PictureState& s, const Pic& a, const Pic& b) {
    if (a.width != b.width || a.height != b.height) {
        say("FAIL %-28s size %ux%u against %ux%u", a.name, a.width, a.height, b.width, b.height);
        return false;
    }
    usize n = (usize)a.width * a.height;
    u32 worst = 0;
    u64 over = 0;
    i64 first = -1;
    for (usize i = 0; i < n; ++i) {
        const u8* pa = a.rgb + i * 3;
        const u8* pb = b.rgb + i * 3;
        u32 d = 0;
        for (u32 k = 0; k < 3; ++k) { u32 x = pa[k] > pb[k] ? pa[k] - pb[k] : pb[k] - pa[k]; if (x > d) d = x; }
        if (d > worst) worst = d;
        if (d > s.threshold) { ++over; if (first < 0) first = (i64)i; }
    }
    f64 fraction = (f64)over / (f64)n;
    bool ok = fraction <= (f64)s.max_fraction && worst <= s.max_channel;
    char where[96] = "";
    if (first >= 0) {
        const u8* pa = a.rgb + first * 3;
        const u8* pb = b.rgb + first * 3;
        snprintf(where, sizeof where, "; first at (%u, %u): (%u, %u, %u) against (%u, %u, %u)", (u32)(first % a.width), (u32)(first / a.width),
                 pa[0], pa[1], pa[2], pb[0], pb[1], pb[2]);
    }
    say("%s %-28s largest channel difference %3u, %llu pixels past %u (%.4f%%)%s", ok ? "ok  " : "FAIL", a.name, worst,
        (unsigned long long)over, s.threshold, fraction * 100.0, where);
    if (s.out[0] && worst) {
        // The difference, amplified eight times.
        u8* diff = (u8*)alloc_in(s.general, n * 3);
        if (!diff) { KV_ERROR("picture", "%s: out of memory for its difference", a.name); ++s.write_failures; return ok; }
        for (usize i = 0; i < n * 3; ++i) {
            u32 x = a.rgb[i] > b.rgb[i] ? a.rgb[i] - b.rgb[i] : b.rgb[i] - a.rgb[i];
            diff[i] = (u8)(x * 8 > 255 ? 255 : x * 8);
        }
        char name[160];
        snprintf(name, sizeof name, "%s.diff.png", a.name);
        write_png(e, s, name, diff, a.width, a.height);
        free_in(s.general, diff, n * 3);
    }
    return ok;
}

// Every picture is read: compare or encode, and submit what is to be written.
void work(Engine& e, PictureState& s) {
    if (s.out[0] && !io_make_dirs(s.out)) { KV_ERROR("picture", "cannot make '%s'", s.out); finish(e, s, 1); return; }
    if (s.op == Op::Png) {
        for (u32 i = 0; i < s.count_a; ++i) {
            const Pic& p = s.pics[i];
            if (!p.ok) continue;
            char name[160];
            snprintf(name, sizeof name, "%s.png", p.name);
            write_png(e, s, name, p.rgb, p.width, p.height);
        }
    } else {
        for (u32 i = 0; i < s.count_a; ++i) {
            const Pic& a = s.pics[i];
            i32 m = s.match[i];
            if (m < 0) { say("FAIL %-28s not in %s", a.name, s.b); ++s.failures; continue; }
            const Pic& b = s.pics[(u32)m];
            if (!a.ok || !b.ok) { say("FAIL %-28s could not be read", a.name); ++s.failures; continue; }
            if (!compare(e, s, a, b)) ++s.failures;
        }
    }
    s.stage = Stage::Write;
}

// Phase PreSim, main thread, every tick: one step of the job.
void picture_pump(TickContext& ctx) {
    PictureState& s = ctx.state_as<PictureState>();
    Engine& e = *ctx.engine;
    IoQueue* io = platform_io(e);
    if (s.op == Op::None || !io) return;

    if (s.stage == Stage::Read) {
        if (s.submitted == 0) {
            for (Pic& p : s.pics) {
                IoRequest r;
                r.path = p.path; r.alloc = s.general; r.user = 2;
                p.io = io->read_file(r);
                if (p.io.is_null()) { KV_ERROR("picture", "cannot read '%s'", p.path); p.done = true; ++s.completed; }
            }
            s.submitted = s.pics.len;
        }
        for (u32 i = 0; i < io->completions().len;) {
            IoCompletion c = io->completions()[i];
            Pic* p = nullptr;
            for (Pic& x : s.pics) if (!x.done && x.io == c.request) { p = &x; break; }
            if (!p) { ++i; continue; }
            io->reap(c.request);
            p->done = true;
            ++s.completed;
            if (c.status != IoStatus::Done) { KV_ERROR("picture", "cannot read '%s' (errno %d)", p->path, c.error); continue; }
            p->ok = decode(s, *p, (const u8*)c.data, c.size);
            free_in(s.general, c.data, c.size ? c.size : 1);
        }
        if (s.completed < s.submitted) return;
        work(e, s);
        if (s.op == Op::None) return; // finished already
    }
    if (s.stage == Stage::Write) {
        for (u32 i = 0; i < io->completions().len;) {
            IoCompletion c = io->completions()[i];
            if (c.user != 1) { ++i; continue; }
            io->reap(c.request);
            if (c.status != IoStatus::Done) { KV_ERROR("picture", "a write failed (errno %d)", c.error); ++s.write_failures; }
            --s.writes_out;
        }
        if (s.writes_out) return;
        u32 unread = 0;
        for (u32 i = 0; i < s.count_a; ++i) if (!s.pics[i].ok) ++unread;
        int code = s.write_failures ? 1 : 0;
        if (s.op == Op::Png) {
            say("%u pictures written to %s", s.count_a - unread, s.out);
            if (unread) code = 1;
        } else {
            say("%u of %u pictures within tolerance", s.count_a - s.failures, s.count_a);
            if (s.failures) code = 1;
        }
        finish(e, s, code);
    }
}

bool picture_startup(Engine&, ModuleInstance& self) {
    PictureState* s = arena_push<PictureState>(self.arena);
    if (!s) return false;
    *s = PictureState{};
    s->general = &self.general;
    s->pics.alloc = &self.general;
    s->match.alloc = &self.general;
    self.set_state(s);
    return true;
}

void picture_shutdown(Engine&, ModuleInstance& self) {
    PictureState* s = self.state_as<PictureState>();
    if (!s) return;
    reset(*s);
    s->pics.free();
    s->match.free();
}

constinit const CliCommandDesc k_commands[] = {
    {.name = "picture compare", .help = "picture compare <a> <b> [--threshold N (8)] [--max-fraction F (0.005)] [--max-channel N (255)] [--diff DIR]: "
                                        "two pictures, or two directories by name, within a tolerance; exit 1 if any pair fails", .fn = cmd_compare},
    {.name = "picture png", .help = "picture png <file or dir> <out dir>: every picture as a PNG, to keep as a reference", .fn = cmd_png},
};
constinit const SystemDesc k_systems[] = {
    {.name = ".pump", .phase = Phase::PreSim, .fn = picture_pump, .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(tool_picture_module) = {
    .name     = "tool_picture",
    .systems  = k_systems,
    .commands = k_commands,
    .startup  = picture_startup,
    .shutdown = picture_shutdown,
    .memory   = {.reserve = 64 * 1024},
};

} // namespace kv
