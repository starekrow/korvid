// kbor: a command names one input and one output; the system reads the input through the
// I/O queue, converts it on the main thread, and writes the output through the queue to
// a temporary name beside it, which is renamed over the output when the write is done, so
// a file is never left half written (normalizing in place included).
//
// The conversion is core's: json_parse or kbor_parse into a tree in an arena sized from
// the input, then kbor_write or json_write. The arena goes when the job does.
#include "korvid/kbor/module.h"
#include "korvid/core/arena.h"
#include "korvid/core/array.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/json.h"
#include "korvid/core/kbor.h"
#include "korvid/core/log.h"
#include "korvid/platform/io.h"
#include "korvid/tool/tool.h"
#include <stdarg.h>
#include <string.h>

namespace kv {

namespace {

enum class Op : u8 { None, Encode, Decode, Normalize, Info };
enum class Stage : u8 { Idle, Read, Write };

struct KborState {
    Allocator* general = nullptr;
    Op         op      = Op::None;
    Stage      stage   = Stage::Idle;
    char       in[1024]  = {};
    char       out[1024] = {};  // empty: stdout (decode)
    char       temp[1100] = {}; // where the write goes before it is renamed to `out`
    bool       compact = false;
    Handle     io;
    usize      in_size = 0, out_size = 0;
};

KborState* state(Engine& e) { return engine_module_state_as<KborState>(e, "kbor"); }

void say(const char* fmt, ...) KV_PRINTF(1, 2);
void say(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprint(fmt, args);
    va_end(args);
    print("\n");
}

void finish(Engine& e, KborState& s, int code) {
    s.op = Op::None;
    s.stage = Stage::Idle;
    s.io = Handle{};
    tool_finish(e, code);
}

bool busy(CliContext& ctx, KborState* s) {
    if (!s) { ctx.fail("the kbor module is not up"); return true; }
    if (s->op != Op::None) { ctx.fail("kbor is busy with another job"); return true; }
    return false;
}

// The words of a command: up to `max` paths, and --compact where `compact` is not null.
bool words(CliContext& ctx, Span<const char* const> args, const char* usage, u32 min, u32 max, const char** paths, bool* compact) {
    u32 n = 0;
    for (u32 i = 0; i < args.len; ++i) {
        const char* a = args[i];
        if (compact && !strcmp(a, "--compact")) { *compact = true; continue; }
        if (a[0] == '-' && a[1] == '-') { ctx.fail("%s: unknown option '%s'", usage, a); return false; }
        if (n == max) { ctx.fail("%s", usage); return false; }
        paths[n++] = a;
    }
    if (n < min) { ctx.fail("%s", usage); return false; }
    return true;
}

bool start(CliContext& ctx, KborState& s, Op op, const char* in, const char* out) {
    if (format(s.in, sizeof s.in, "%s", in) >= (i32)sizeof s.in || format(s.out, sizeof s.out, "%s", out ? out : "") >= (i32)sizeof s.out) {
        ctx.fail("path too long");
        return false;
    }
    s.op = op;
    s.stage = Stage::Read;
    s.io = Handle{};
    return true;
}

// ---- commands ----------------------------------------------------------------------------------

void cmd_encode(CliContext& ctx, Span<const char* const> args) {
    KborState* s = state(*ctx.engine);
    if (busy(ctx, s)) return;
    const char* p[2] = {};
    if (!words(ctx, args, "kbor encode <in.json> <out.kbor>", 2, 2, p, nullptr)) return;
    start(ctx, *s, Op::Encode, p[0], p[1]);
}

void cmd_decode(CliContext& ctx, Span<const char* const> args) {
    KborState* s = state(*ctx.engine);
    if (busy(ctx, s)) return;
    const char* p[2] = {};
    s->compact = false;
    if (!words(ctx, args, "kbor decode <in.kbor> [out.json] [--compact]", 1, 2, p, &s->compact)) return;
    start(ctx, *s, Op::Decode, p[0], p[1]);
}

void cmd_normalize(CliContext& ctx, Span<const char* const> args) {
    KborState* s = state(*ctx.engine);
    if (busy(ctx, s)) return;
    const char* p[2] = {};
    if (!words(ctx, args, "kbor normalize <in.kbor> [out.kbor]", 1, 2, p, nullptr)) return;
    start(ctx, *s, Op::Normalize, p[0], p[1] ? p[1] : p[0]);
}

void cmd_info(CliContext& ctx, Span<const char* const> args) {
    KborState* s = state(*ctx.engine);
    if (busy(ctx, s)) return;
    const char* p[1] = {};
    if (!words(ctx, args, "kbor info <in.kbor>", 1, 1, p, nullptr)) return;
    start(ctx, *s, Op::Info, p[0], nullptr);
}

// ---- the work ----------------------------------------------------------------------------------

// The output's bytes: to stdout, or a write to the temporary name. False when it failed.
bool emit(Engine& e, KborState& s, const Array<u8>& bytes) {
    s.out_size = bytes.len;
    if (!s.out[0]) {
        output_write(false, (const char*)bytes.data, bytes.len);
        print_flush();
        return true;
    }
    format(s.temp, sizeof s.temp, "%s.kbor-tmp", s.out);
    IoWriteRequest w;
    w.path = s.temp; w.data = bytes.data; w.size = bytes.len; w.alloc = s.general;
    s.io = platform_io(e)->write_file(w);
    if (s.io.is_null()) { KV_ERROR("kbor", "cannot write '%s'", s.temp); return false; }
    s.stage = Stage::Write;
    return true;
}

// The input is read: parse it and make the output. 0, or the exit status to finish with.
int convert(Engine& e, KborState& s, const u8* data, usize size) {
    // A tree is at most a node (24 bytes) per input byte, and its strings a few bytes more
    // than the input's: the arena reserves that much address space and commits what it uses.
    Arena arena;
    if (!arena_create(arena, size * 32 + (1u << 20), "kbor", MemTag{})) { KV_ERROR("kbor", "out of memory for '%s'", s.in); return 1; }
    JsonDoc doc;
    KborInfo info;
    bool ok = s.op == Op::Encode ? json_parse((const char*)data, size, &arena.a, doc) : kbor_parse(data, size, &arena.a, doc, &info);
    int code = 0;
    if (!ok) {
        if (s.op == Op::Encode) KV_ERROR("kbor", "%s:%u: %s", s.in, doc.error_line, doc.error);
        else KV_ERROR("kbor", "%s: %s at byte %u", s.in, doc.error, doc.error_at);
        code = 1;
    } else if (s.op == Op::Info) {
        say("%s: %llu bytes, %s%s", s.in, (unsigned long long)size, info.summarized ? "normalized (summary)" : "not normalized (no summary)",
            info.indefinite ? ", indefinite lengths" : "");
        say("  values %llu, text keys %llu, index keys %llu, distinct names %llu",
            (unsigned long long)info.counts.nodes, (unsigned long long)info.counts.keys, (unsigned long long)info.index_keys, (unsigned long long)info.names);
        say("  strings %llu, string bytes %llu", (unsigned long long)info.counts.strings, (unsigned long long)info.counts.bytes);
        say("  root: %s of %u", json_type_name(doc.root().type), doc.root().count());
    } else {
        Array<u8> bytes;
        bytes.alloc = s.general;
        if (s.op == Op::Decode) json_write(doc, bytes, s.compact ? 0 : KV_JSON_WRITE_PRETTY);
        else kbor_write(doc, bytes);
        // Normalizing a file that is already normalized changes nothing: nothing is written.
        if (s.op == Op::Normalize && !strcmp(s.in, s.out) && bytes.len == size && memcmp(bytes.data, data, size) == 0) {
            say("%s: already normalized, %llu bytes", s.in, (unsigned long long)size);
            s.out[0] = 0;
            s.op = Op::Info; // finished below, as info is
        } else if (!emit(e, s, bytes)) code = 1;
        bytes.free(); // the write took a copy
    }
    arena_destroy(arena);
    return code;
}

// Phase PreSim, main thread, every tick: one step of the job.
void kbor_pump(TickContext& ctx) {
    KborState& s = ctx.state_as<KborState>();
    Engine& e = *ctx.engine;
    IoQueue* io = platform_io(e);
    if (s.op == Op::None || !io) return;

    if (s.stage == Stage::Read) {
        if (s.io.is_null()) {
            IoRequest r;
            r.path = s.in; r.alloc = s.general;
            s.io = io->read_file(r);
            if (s.io.is_null()) { KV_ERROR("kbor", "cannot read '%s'", s.in); finish(e, s, 1); }
            return;
        }
        for (const IoCompletion& c : io->completions()) {
            if (c.request != s.io) continue;
            IoCompletion done = c;
            io->reap(done.request);
            s.io = Handle{};
            if (done.status != IoStatus::Done) { KV_ERROR("kbor", "cannot read '%s' (errno %d)", s.in, done.error); finish(e, s, 1); return; }
            s.in_size = done.size;
            int code = convert(e, s, (const u8*)done.data, done.size);
            free_in(s.general, done.data, done.size ? done.size : 1);
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
            if (done.status != IoStatus::Done) {
                KV_ERROR("kbor", "cannot write '%s' (errno %d)", s.temp, done.error);
                io_remove(s.temp);
                finish(e, s, 1);
                return;
            }
            if (!io_rename(s.temp, s.out)) {
                KV_ERROR("kbor", "cannot rename '%s' to '%s'", s.temp, s.out);
                io_remove(s.temp);
                finish(e, s, 1);
                return;
            }
            say("%s: %llu bytes from %llu", s.out, (unsigned long long)s.out_size, (unsigned long long)s.in_size);
            finish(e, s, 0);
            return;
        }
    }
}

bool kbor_startup(Engine&, ModuleInstance& self) {
    KborState* s = arena_push<KborState>(self.arena);
    if (!s) return false;
    *s = KborState{};
    s->general = &self.general;
    self.set_state(s);
    return true;
}

constinit const CliCommandDesc k_commands[] = {
    {.name = "kbor encode", .help = "kbor encode <in.json> <out.kbor>: JSON as normalized KBOR", .fn = cmd_encode},
    {.name = "kbor decode", .help = "kbor decode <in.kbor> [out.json] [--compact]: KBOR as JSON, to stdout without a file", .fn = cmd_decode},
    {.name = "kbor normalize", .help = "kbor normalize <in.kbor> [out.kbor]: KBOR in its tightest form (an index for every repeated key, "
                                       "definite lengths, the summary in front); in place without an output", .fn = cmd_normalize},
    {.name = "kbor info", .help = "kbor info <in.kbor>: whether it is normalized, and what it holds", .fn = cmd_info},
};
constinit const SystemDesc k_systems[] = {
    {.name = ".pump", .phase = Phase::PreSim, .fn = kbor_pump, .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(kbor_module) = {
    .name     = "kbor",
    .systems  = k_systems,
    .commands = k_commands,
    .startup  = kbor_startup,
    .memory   = {.reserve = 64 * 1024},
};

} // namespace kv
