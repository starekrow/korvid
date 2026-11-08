// kpack: commands start a job, the system runs it. A job is a list of items (files to
// read, or a pack's entries to read back), read through the I/O queue within its
// budgets, then packed, written, checked or extracted.
#include "korvid/tool_kpack/module.h"
#include "korvid/core/array.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/inflate/inflate.h"
#include "korvid/pack/zip.h"
#include "korvid/platform/io.h"
#include "korvid/tool/tool.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace kv {

namespace {

enum class Op : u8 { None, Pack, Verify, Extract, Repack };
enum class Stage : u8 { Idle, Walk, ReadList, Read, Write, Done };

struct Item {
    char   name[KV_PACK_PATH_MAX]; // the logical name in the pack
    char   path[1100];             // what to read: a file (pack), or the name again (from a mount)
    Handle io;
    u8*    data = nullptr;
    usize  size = 0;
    u32    crc  = 0;               // from the source pack's table, for verify
    bool   from_mount = false;
    bool   done = false, failed = false;
};

struct KpackState {
    Allocator*  general = nullptr;
    Op          op      = Op::None;
    Stage       stage   = Stage::Idle;
    char        out[512] = {}, root[512] = {}, list[512] = {}, source[512] = {};
    Array<Item> items;
    Array<u8>   archive;
    Handle      list_read, write_req;
    u32         submitted = 0, completed = 0, failures = 0, writes_out = 0;
    u64         bytes = 0;
};

KpackState* state(Engine& e) { return engine_module_state_as<KpackState>(e, "tool_kpack"); }

void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void say(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    fputc('\n', stdout);
}

void reset(KpackState& s) {
    for (Item& it : s.items) free_in(s.general, it.data, it.size ? it.size : 1);
    s.items.clear();
    s.archive.free();
    s.op = Op::None; s.stage = Stage::Idle;
    s.submitted = s.completed = s.failures = s.writes_out = 0;
    s.bytes = 0;
    s.list_read = s.write_req = Handle{};
}

void finish(Engine& e, KpackState& s, int code) {
    reset(s);
    tool_finish(e, code);
}

bool busy(CliContext& ctx, KpackState* s) {
    if (!s) { ctx.fail("the kpack module is not up"); return true; }
    if (s->op != Op::None) { ctx.fail("kpack is busy with another job"); return true; }
    return false;
}

// Adds a file of the staged tree: the name is the path below `root`.
bool add_file(KpackState& s, const char* path) {
    usize rl = strlen(s.root);
    const char* rel = path;
    if (rl && strncmp(path, s.root, rl) == 0 && (path[rl] == '/' || s.root[rl - 1] == '/')) rel = path + rl + (path[rl] == '/' ? 1 : 0);
    Item it{};
    if (!pack_normalize(rel, it.name, sizeof it.name)) { KV_ERROR("kpack", "'%s' is not a usable name under '%s'", path, s.root); return false; }
    int n = path[0] == '/' || rl == 0 || rel != path ? snprintf(it.path, sizeof it.path, "%s", path)
                                                     : snprintf(it.path, sizeof it.path, "%s/%s", s.root, path);
    if (n < 0 || (usize)n >= sizeof it.path) { KV_ERROR("kpack", "path too long: '%s'", path); return false; }
    s.items.push(it);
    return true;
}

bool add_listed(void* user, const char* relative) {
    KpackState& s = *(KpackState*)user;
    Item it{};
    if (!pack_normalize(relative, it.name, sizeof it.name)) return true; // not a usable name: skipped
    int n = snprintf(it.path, sizeof it.path, "%s/%s", s.root, relative);
    if (n < 0 || (usize)n >= sizeof it.path) return true; // too long to read: skipped
    s.items.push(it);
    return true;
}

// Mounts a pack for reading back and makes an item per entry, the reserved ones left out.
bool items_from_pack(CliContext& ctx, KpackState& s, const char* path) {
    IoQueue* io = platform_io(*ctx.engine);
    if (!io || !io->mount_pack(path)) { ctx.fail("'%s' is not a readable zip", path); return false; }
    const PackTable& t = io->mounts[io->mounts.len - 1].table;
    for (u32 i = 0; i < t.count; ++i) {
        const char* name = pack_entry_name(t, t.entries[i]);
        if (strncmp(name, ".kpak/", 6) == 0) continue;
        Item it{};
        snprintf(it.name, sizeof it.name, "%s", name);
        snprintf(it.path, sizeof it.path, "%s", name);
        it.crc = t.entries[i].crc;
        it.from_mount = true;
        s.items.push(it);
    }
    snprintf(s.source, sizeof s.source, "%s", path);
    return true;
}

// ---- commands ----------------------------------------------------------------------------------

// kpack pack <out.kpak> <root> [file | @list]...
void cmd_pack(CliContext& ctx, Span<const char* const> args) {
    KpackState* s = state(*ctx.engine);
    if (busy(ctx, s)) return;
    if (args.len < 2) { ctx.fail("kpack pack <out.kpak> <root dir> [file | @listfile]...: no files means every file under root"); return; }
    snprintf(s->out, sizeof s->out, "%s", args[0]);
    snprintf(s->root, sizeof s->root, "%s", args[1]);
    usize rl = strlen(s->root);
    while (rl > 1 && s->root[rl - 1] == '/') s->root[--rl] = 0;
    s->op = Op::Pack;
    s->stage = args.len == 2 ? Stage::Walk : Stage::Read;
    for (u32 i = 2; i < args.len; ++i) {
        if (args[i][0] == '@') {
            if (s->list[0]) { ctx.fail("one @listfile per run"); reset(*s); return; }
            snprintf(s->list, sizeof s->list, "%s", args[i] + 1);
            s->stage = Stage::ReadList;
        } else if (!add_file(*s, args[i])) { ctx.fail("bad file name '%s'", args[i]); reset(*s); return; }
    }
}

// kpack list <pack>
void cmd_list(CliContext& ctx, Span<const char* const> args) {
    KpackState* s = state(*ctx.engine);
    if (busy(ctx, s)) return;
    if (args.len != 1) { ctx.fail("kpack list <pack>"); return; }
    IoQueue* io = platform_io(*ctx.engine);
    if (!io || !io->mount_pack(args[0])) { ctx.fail("'%s' is not a readable zip", args[0]); return; }
    const PackTable& t = io->mounts[io->mounts.len - 1].table;
    ctx.print("%s: %u entries, %s; %u deflated, %u unaligned, %u unsupported", args[0], t.count, t.indexed ? "indexed" : "no index",
              t.deflated, t.unaligned, t.unsupported);
    for (u32 i = 0; i < t.count; ++i) {
        const PackEntry& e = t.entries[i];
        char where[32];
        if (e.data_offset) snprintf(where, sizeof where, "@%llu%s", (unsigned long long)e.data_offset, e.data_offset % KV_PACK_ALIGN ? "" : " aligned");
        else snprintf(where, sizeof where, "header @%llu", (unsigned long long)e.header_offset);
        ctx.print("%10llu %10llu %-7s %08x %-20s %s", (unsigned long long)e.size, (unsigned long long)e.csize,
                  e.method == ZipStored ? "stored" : e.method == ZipDeflate ? "deflate" : "other", e.crc, where, pack_entry_name(t, e));
    }
    tool_finish(*ctx.engine, 0);
}

void start_readback(CliContext& ctx, Op op, Span<const char* const> args, u32 want, const char* usage) {
    KpackState* s = state(*ctx.engine);
    if (busy(ctx, s)) return;
    if (args.len != want) { ctx.fail("%s", usage); return; }
    if (!items_from_pack(ctx, *s, args[0])) { reset(*s); return; }
    if (want > 1) snprintf(s->out, sizeof s->out, "%s", args[1]);
    s->op = op;
    s->stage = Stage::Read;
}

void cmd_verify(CliContext& ctx, Span<const char* const> args) { start_readback(ctx, Op::Verify, args, 1, "kpack verify <pack>"); }
void cmd_extract(CliContext& ctx, Span<const char* const> args) { start_readback(ctx, Op::Extract, args, 2, "kpack extract <pack> <dir>"); }
void cmd_repack(CliContext& ctx, Span<const char* const> args) { start_readback(ctx, Op::Repack, args, 2, "kpack repack <zip> <out.kpak>"); }

// ---- the job -----------------------------------------------------------------------------------

bool collect(void* user, const void* p, usize n) {
    Array<u8>& a = *(Array<u8>*)user;
    usize at = a.len;
    if (at + n > 0xFFFFFFFFull) return false; // Array counts in 32 bits: a pack this size needs a streaming write
    a.reserve((u32)(at + n));
    if (!a.data) return false;
    memcpy(a.data + at, p, n);
    a.len = (u32)(at + n);
    return true;
}

// The items are all read: make the archive and submit its write.
void write_archive(Engine& e, KpackState& s) {
    IoQueue& io = *platform_io(e);
    Array<PackWriteFile> files;
    files.alloc = s.general;
    for (const Item& it : s.items) files.push(PackWriteFile{it.name, it.data, it.size});
    MemScope scope;
    const char* why = nullptr;
    s.archive.alloc = s.general;
    bool ok = kpak_write(files.span(), collect, &s.archive, mem_scratch(), &why);
    files.free();
    if (!ok) { KV_ERROR("kpack", "%s: %s", s.out, why); finish(e, s, 1); return; }
    IoWriteRequest w;
    w.path = s.out;
    w.data = s.archive.data;
    w.size = s.archive.len;
    w.alloc = s.general;
    w.user = 1;
    s.write_req = io.write_file(w);
    if (s.write_req.is_null()) { KV_ERROR("kpack", "cannot write '%s'", s.out); finish(e, s, 1); return; }
    s.stage = Stage::Write;
}

// What a finished read means for each job.
void on_read(Engine& e, KpackState& s, Item& it) {
    IoQueue& io = *platform_io(e);
    if (s.op == Op::Verify) {
        if (crc32(0, it.data, it.size) != it.crc) { KV_ERROR("kpack", "%s: CRC-32 mismatch", it.name); ++s.failures; }
        free_in(s.general, it.data, it.size ? it.size : 1);
        it.data = nullptr;
    } else if (s.op == Op::Extract) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", s.out, it.name);
        if (char* slash = strrchr(path, '/')) { *slash = 0; io_make_dirs(path); *slash = '/'; }
        IoWriteRequest w;
        w.path = path; w.data = it.data; w.size = it.size; w.alloc = s.general; w.user = 2;
        if (io.write_file(w).is_null()) { KV_ERROR("kpack", "cannot write '%s'", path); ++s.failures; }
        else ++s.writes_out;
        free_in(s.general, it.data, it.size ? it.size : 1); // the write took a copy
        it.data = nullptr;
    }
}

// Phase PreSim, main thread, every tick: one step of the job.
void kpack_pump(TickContext& ctx) {
    KpackState& s = ctx.state_as<KpackState>();
    Engine& e = *ctx.engine;
    IoQueue* io = platform_io(e);
    if (s.op == Op::None || !io) return;

    if (s.stage == Stage::Walk) {
        if (io_list_files(s.root, add_listed, &s) < 0) { KV_ERROR("kpack", "cannot read '%s'", s.root); finish(e, s, 1); return; }
        s.stage = Stage::Read;
    }
    if (s.stage == Stage::ReadList) {
        if (s.list_read.is_null()) {
            IoRequest r;
            r.path = s.list; r.alloc = s.general; r.user = 3;
            s.list_read = io->read_file(r);
            if (s.list_read.is_null()) { KV_ERROR("kpack", "cannot read '%s'", s.list); finish(e, s, 1); return; }
        }
        for (const IoCompletion& c : io->completions()) {
            if (c.request != s.list_read) continue;
            IoCompletion done = c;
            io->reap(done.request);
            if (done.status != IoStatus::Done) { KV_ERROR("kpack", "cannot read '%s'", s.list); finish(e, s, 1); return; }
            // Whitespace-separated paths, as Ninja writes a response file.
            char* text = (char*)done.data;
            bool ok = true;
            for (usize at = 0; at < done.size && ok;) {
                while (at < done.size && (text[at] == ' ' || text[at] == '\n' || text[at] == '\t' || text[at] == '\r')) ++at;
                usize start = at;
                while (at < done.size && !(text[at] == ' ' || text[at] == '\n' || text[at] == '\t' || text[at] == '\r')) ++at;
                if (at == start) break;
                char path[512];
                usize n = at - start < sizeof path - 1 ? at - start : sizeof path - 1;
                memcpy(path, text + start, n);
                path[n] = 0;
                ok = add_file(s, path);
            }
            free_in(s.general, done.data, done.size ? done.size : 1);
            if (!ok) { finish(e, s, 1); return; }
            s.stage = Stage::Read;
            break;
        }
        if (s.stage == Stage::ReadList) return;
    }
    if (s.stage == Stage::Read) {
        if (s.submitted == 0) {
            for (Item& it : s.items) {
                IoRequest r;
                r.path = it.path; r.alloc = s.general; r.user = 4;
                it.io = it.from_mount ? io->read_asset(r) : io->read_file(r);
                if (it.io.is_null()) { it.done = it.failed = true; ++s.failures; ++s.completed; }
            }
            s.submitted = s.items.len ? s.items.len : 1;
            if (!s.items.len) s.completed = 1;
        }
        for (u32 i = 0; i < io->completions().len;) {
            IoCompletion c = io->completions()[i];
            Item* it = nullptr;
            for (Item& x : s.items) if (!x.done && x.io == c.request) { it = &x; break; }
            if (!it) { ++i; continue; }
            io->reap(c.request);
            it->done = true;
            ++s.completed;
            if (c.status != IoStatus::Done) { KV_ERROR("kpack", "cannot read '%s' (errno %d)", it->path, c.error); it->failed = true; ++s.failures; continue; }
            it->data = (u8*)c.data;
            it->size = c.size;
            s.bytes += c.size;
            on_read(e, s, *it);
        }
        if (s.completed < s.submitted) return;
        if (s.op == Op::Pack || s.op == Op::Repack) {
            if (s.failures) { finish(e, s, 1); return; }
            write_archive(e, s);
            return;
        }
        s.stage = Stage::Write; // verify has nothing to write; extract waits for its writes
    }
    if (s.stage == Stage::Write) {
        for (u32 i = 0; i < io->completions().len;) {
            IoCompletion c = io->completions()[i];
            if (c.user != 1 && c.user != 2) { ++i; continue; }
            io->reap(c.request);
            if (c.status != IoStatus::Done) { KV_ERROR("kpack", "a write failed (errno %d)", c.error); ++s.failures; }
            if (c.user == 2) --s.writes_out;
            if (c.user == 1) s.write_req = Handle{};
        }
        if (!s.write_req.is_null() || s.writes_out) return;
        switch (s.op) {
        case Op::Pack: case Op::Repack:
            if (!s.failures) say("%s: %u files, %llu bytes in, %u bytes out", s.out, s.items.len, (unsigned long long)s.bytes, s.archive.len);
            break;
        case Op::Verify:
            say("%s: %u entries, %llu bytes, %u bad", s.source, s.items.len, (unsigned long long)s.bytes, s.failures);
            break;
        case Op::Extract:
            say("%s: %u files into %s", s.source, s.items.len, s.out);
            break;
        default: break;
        }
        finish(e, s, s.failures ? 1 : 0);
    }
}

bool kpack_startup(Engine&, ModuleInstance& self) {
    KpackState* s = arena_push<KpackState>(self.arena);
    if (!s) return false;
    s->general = &self.general;
    s->items.alloc = &self.general;
    self.set_state(s);
    return true;
}

void kpack_shutdown(Engine&, ModuleInstance& self) {
    KpackState* s = self.state_as<KpackState>();
    if (!s) return;
    reset(*s);
    s->items.free();
}

constinit const CliCommandDesc k_commands[] = {
    {.name = "kpack pack", .help = "kpack pack <out.kpak> <root> [file | @list]...: a pack of the files (all under root if none named)", .fn = cmd_pack},
    {.name = "kpack list", .help = "kpack list <pack>: every entry with its size, method, CRC and offset", .fn = cmd_list},
    {.name = "kpack verify", .help = "kpack verify <pack>: read every entry back and check its CRC-32", .fn = cmd_verify},
    {.name = "kpack extract", .help = "kpack extract <pack> <dir>: every entry as a file under dir", .fn = cmd_extract},
    {.name = "kpack repack", .help = "kpack repack <zip> <out.kpak>: any zip as an aligned, indexed pack", .fn = cmd_repack},
};
constinit const SystemDesc k_systems[] = {
    {.name = ".pump", .phase = Phase::PreSim, .fn = kpack_pump, .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(tool_kpack_module) = {
    .name     = "tool_kpack",
    .systems  = k_systems,
    .commands = k_commands,
    .startup  = kpack_startup,
    .shutdown = kpack_shutdown,
    .memory   = {.reserve = 64 * 1024},
};

} // namespace kv
