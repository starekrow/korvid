// kimc: a command names its input and output; the system reads the input through the I/O
// queue, compiles it on the main thread (kim_compile), and writes the .kimb through the
// queue to a temporary name beside it, renamed over the output when the write is done.
// Errors go to stderr one to a line, file:line:col: text, as a compiler's do, so an
// editor can jump to them.
#include "korvid/kimc/module.h"
#include "korvid/core/arena.h"
#include "korvid/core/array.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/kim/kim.h"
#include "korvid/kimb/kimb.h"
#include "korvid/platform/io.h"
#include "korvid/tool/tool.h"
#include <string.h>

namespace kv {

namespace {

enum class Op : u8 { None, Compile, Check };
enum class Stage : u8 { Idle, Read, Write };

struct KimcState {
    Allocator* general = nullptr;
    Op         op      = Op::None;
    Stage      stage   = Stage::Idle;
    char       in[1024]   = {};
    char       out[1024]  = {};
    char       temp[1100] = {};
    char       as[1024]   = {}; // the file name the document records; the input's path when empty
    u32        flags      = 0;
    char       roots[2048] = {};  // check: NAME=DIR and DIR, NUL separated, where uses are found
    u32        root_count  = 0;
    Handle     io;
    usize      in_size = 0, out_size = 0;
};

KimcState* state(Engine& e) { return engine_module_state_as<KimcState>(e, "kimc"); }

void finish(Engine& e, KimcState& s, int code) {
    s.op = Op::None;
    s.stage = Stage::Idle;
    s.io = Handle{};
    tool_finish(e, code);
}

bool busy(CliContext& ctx, KimcState* s) {
    if (!s) { ctx.fail("the kimc module is not up"); return true; }
    if (s->op != Op::None) { ctx.fail("kimc is busy with another job"); return true; }
    return false;
}

void cmd_compile(CliContext& ctx, Span<const char* const> args) {
    KimcState* s = state(*ctx.engine);
    if (busy(ctx, s)) return;
    const char* usage = "kimc compile <in.kim> <out.kimb> [--no-lines] [--as asset/path.kim]";
    const char* p[2] = {};
    u32 n = 0;
    s->flags = 0;
    s->as[0] = 0;
    for (u32 i = 0; i < args.len; ++i) {
        const char* a = args[i];
        if (!strcmp(a, "--no-lines")) { s->flags |= KV_KIM_NO_LINES; continue; }
        if (!strcmp(a, "--as")) {
            if (i + 1 == args.len) { ctx.fail("%s", usage); return; }
            format(s->as, sizeof s->as, "%s", args[++i]);
            continue;
        }
        if (a[0] == '-' && a[1] == '-') { ctx.fail("%s: unknown option '%s'", usage, a); return; }
        if (n == 2) { ctx.fail("%s", usage); return; }
        p[n++] = a;
    }
    if (n < 2) { ctx.fail("%s", usage); return; }
    if (format(s->in, sizeof s->in, "%s", p[0]) >= (i32)sizeof s->in || format(s->out, sizeof s->out, "%s", p[1]) >= (i32)sizeof s->out) {
        ctx.fail("path too long");
        return;
    }
    s->op = Op::Compile;
    s->stage = Stage::Read;
    s->io = Handle{};
}

void cmd_check(CliContext& ctx, Span<const char* const> args) {
    KimcState* s = state(*ctx.engine);
    if (busy(ctx, s)) return;
    const char* usage = "kimc check <in.kim> [--root NAME=DIR]... [--tree DIR]... [--as asset/path.kim]";
    const char* in = nullptr;
    s->as[0] = 0;
    s->root_count = 0;
    usize used = 0;
    for (u32 i = 0; i < args.len; ++i) {
        const char* a = args[i];
        if (!strcmp(a, "--root") || !strcmp(a, "--tree")) {
            if (i + 1 == args.len) { ctx.fail("%s", usage); return; }
            const char* v = args[++i];
            bool named = !strcmp(a, "--root");
            if (named && !strchr(v, '=')) { ctx.fail("%s: --root takes NAME=DIR", usage); return; }
            usize n = strlen(v) + 1 + (named ? 0 : 1);
            if (used + n >= sizeof s->roots) { ctx.fail("too many roots"); return; }
            // A tree is stored as "=DIR": no name, every use under it by its whole path.
            if (!named) s->roots[used++] = '=';
            memcpy(s->roots + used, v, strlen(v) + 1);
            used += strlen(v) + 1;
            ++s->root_count;
            continue;
        }
        if (!strcmp(a, "--as")) {
            if (i + 1 == args.len) { ctx.fail("%s", usage); return; }
            format(s->as, sizeof s->as, "%s", args[++i]);
            continue;
        }
        if (a[0] == '-' && a[1] == '-') { ctx.fail("%s: unknown option '%s'", usage, a); return; }
        if (in) { ctx.fail("%s", usage); return; }
        in = a;
    }
    if (!in) { ctx.fail("%s", usage); return; }
    if (format(s->in, sizeof s->in, "%s", in) >= (i32)sizeof s->in) { ctx.fail("path too long"); return; }
    s->op = Op::Check;
    s->stage = Stage::Read;
    s->io = Handle{};
}

void report(const KimErrors& errors) {
    for (u32 i = 0; i < errors.count; ++i) {
        char line[512];
        errors.format_one(i, line, sizeof line);
        print_err("%s\n", line);
    }
    if (errors.total > errors.count) print_err("... and %u more\n", errors.total - errors.count);
    print_flush();
}

// The input is read: compile it and start the write. 0, or the exit status to finish with.
int compile(Engine& e, KimcState& s, const u8* data, usize size) {
    Array<u8> bytes;
    bytes.alloc = s.general;
    KimErrors errors;
    if (!kim_compile((const char*)data, size, s.as[0] ? s.as : s.in, bytes, errors, s.flags)) {
        report(errors);
        bytes.free();
        return 1;
    }
    s.out_size = bytes.len;
    format(s.temp, sizeof s.temp, "%s.kimc-tmp", s.out);
    IoWriteRequest w;
    w.path = s.temp; w.data = bytes.data; w.size = bytes.len; w.alloc = s.general;
    s.io = platform_io(e)->write_file(w);
    bytes.free(); // the write took a copy
    if (s.io.is_null()) { KV_ERROR("kimc", "cannot write '%s'", s.temp); return 1; }
    s.stage = Stage::Write;
    return 0;
}

// ---- check -------------------------------------------------------------------------------------
// The input and every prelude it uses, read as they are found (a handful of small files:
// read in place, not queued), each as .kim text or a .kimb.

struct CheckJob {
    KimcState* s = nullptr;
    Arena*     arena = nullptr;
    KimErrors* errors = nullptr;
    KimbFile   files[KV_KIMB_MAX_SCOPE];
    char       names[KV_KIMB_MAX_SCOPE][256] = {};
    u32        count = 0;
};

// Reads one file as .kim (by its name) or .kimb into the job. Null when it is not there.
const KimbFile* load(CheckJob& j, const char* path, const char* as) {
    if (j.count == KV_KIMB_MAX_SCOPE) return nullptr;
    IoBytes b;
    if (!io_read_file(path, &b)) return nullptr;
    KimbFile& f = j.files[j.count];
    format(j.names[j.count], sizeof j.names[0], "%s", as);
    const char* name = j.names[j.count];
    usize n = strlen(path);
    bool ok;
    if (n > 4 && !strcmp(path + n - 4, ".kim")) {
        JsonDoc doc;
        ok = kim_parse((const char*)b.data, b.size, name, &j.arena->a, doc, *j.errors) && kimb_from_doc(doc, f, *j.errors, name);
    } else {
        ok = kimb_read(b.data, b.size, &j.arena->a, f, *j.errors, name);
    }
    b.free();
    if (!ok) return nullptr;
    ++j.count;
    return &f;
}

const KimbFile* find_use(void* user, const char* use) {
    CheckJob& j = *(CheckJob*)user;
    for (u32 i = 0; i < j.count; ++i) {
        char want[300];
        format(want, sizeof want, "%s.kim", use);
        if (!strcmp(j.names[i], want)) return &j.files[i];
    }
    const char* r = j.s->roots;
    for (u32 i = 0; i < j.s->root_count; ++i, r += strlen(r) + 1) {
        const char* eq = strchr(r, '=');
        const char* dir = eq + 1;
        const char* rest = use;
        if (eq != r) { // NAME=DIR: the uses under NAME/
            usize nl = (usize)(eq - r);
            if (strncmp(use, r, nl) != 0 || use[nl] != '/') continue;
            rest = use + nl + 1;
        }
        char path[1400], as[300];
        format(as, sizeof as, "%s.kim", use);
        static const char* const exts[] = {".kim", ".kimb"};
        for (const char* ext : exts) {
            format(path, sizeof path, "%s/%s%s", dir, rest, ext);
            if (io_is_file(path)) return load(j, path, as);
        }
    }
    return nullptr;
}

int check(KimcState& s) {
    Arena arena;
    if (!arena_create(arena, 256u << 20, "kimc", MemTag{})) { KV_ERROR("kimc", "out of memory"); return 1; }
    KimErrors errors;
    CheckJob j;
    j.s = &s;
    j.arena = &arena;
    j.errors = &errors;
    const KimbFile* f = load(j, s.in, s.as[0] ? s.as : s.in);
    int code = 0;
    if (!f) {
        if (!errors.total) KV_ERROR("kimc", "cannot read '%s'", s.in);
        code = 1;
    } else {
        KimbScope scope;
        kimb_scope(scope, *f, find_use, &j, errors);
        kimb_check(scope, errors);
        if (!errors.total) print("%s: no mistakes (%u file%s)\n", f->file, scope.count, scope.count == 1 ? "" : "s");
    }
    if (errors.total) { report(errors); code = 1; }
    arena_destroy(arena);
    return code;
}

// Phase PreSim, main thread, every tick: one step of the job.
void kimc_pump(TickContext& ctx) {
    KimcState& s = ctx.state_as<KimcState>();
    Engine& e = *ctx.engine;
    IoQueue* io = platform_io(e);
    if (s.op == Op::None || !io) return;
    if (s.op == Op::Check) { finish(e, s, check(s)); return; }

    if (s.stage == Stage::Read) {
        if (s.io.is_null()) {
            IoRequest r;
            r.path = s.in; r.alloc = s.general;
            s.io = io->read_file(r);
            if (s.io.is_null()) { KV_ERROR("kimc", "cannot read '%s'", s.in); finish(e, s, 1); }
            return;
        }
        if (IoCompletion done; io->take(s.io, &done)) {
            s.io = Handle{};
            if (done.status != IoStatus::Done) { KV_ERROR("kimc", "cannot read '%s' (errno %d)", s.in, done.error); finish(e, s, 1); return; }
            s.in_size = done.bytes.size;
            int code = compile(e, s, (const u8*)done.bytes.data, done.bytes.size);
            done.bytes.free();
            if (code || s.stage != Stage::Write) finish(e, s, code);
            return;
        }
        return;
    }

    if (s.stage == Stage::Write) {
        for (const IoCompletion& c : io->completions()) {
            if (c.request != s.io) continue;
            IoCompletion done = c;
            io->reap(done.request);
            if (done.status != IoStatus::Done || !io_rename(s.temp, s.out)) {
                KV_ERROR("kimc", "cannot write '%s'", s.out);
                io_remove(s.temp);
                finish(e, s, 1);
                return;
            }
            print("%s: %llu bytes from %llu\n", s.out, (unsigned long long)s.out_size, (unsigned long long)s.in_size);
            finish(e, s, 0);
            return;
        }
    }
}

bool kimc_startup(Engine&, ModuleInstance& self) {
    KimcState* s = arena_push<KimcState>(self.arena);
    if (!s) return false;
    *s = KimcState{};
    s->general = &self.general;
    self.set_state(s);
    return true;
}

constinit const CliCommandDesc k_commands[] = {
    {.name = "kimc check", .help = "kimc check <in.kim> [--root NAME=DIR]... [--tree DIR]... [--as asset/path.kim]: every mistake, by the rules Krome loads "
                                   "by; a use NAME/x is NAME=DIR's x.kim or x.kimb, or a tree's NAME/x.kimb", .fn = cmd_check},
    {.name = "kimc compile", .help = "kimc compile <in.kim> <out.kimb> [--no-lines] [--as asset/path.kim]: .kim text as a canonical .kimb", .fn = cmd_compile},
};
constinit const SystemDesc k_systems[] = {
    {.name = ".pump", .phase = Phase::PreSim, .fn = kimc_pump, .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(kimc_module) = {
    .name     = "kimc",
    .systems  = k_systems,
    .commands = k_commands,
    .startup  = kimc_startup,
    .memory   = {.reserve = 64 * 1024},
};

} // namespace kv
