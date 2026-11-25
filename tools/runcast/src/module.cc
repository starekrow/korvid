// runcast: serve a child process's output over a loopback socket, and fetch it.
// Written from the public headers only, as a cold read of the platform layer.
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/os_error.h"
#include "korvid/core/os_process.h"
#include "korvid/core/time.h"
#include "korvid/platform/module.h"
#include "korvid/platform/process.h"
#include "korvid/runcast/module.h"
#include "korvid/tool/tool.h"
#include <errno.h>
#include <string.h>

namespace kv {

namespace {

enum class Stage : u8 {
    Idle,
    // serve
    Listening,  // waiting for the one client
    Streaming,  // the child runs; its output goes to the client
    Draining,   // the child ended and its pipes are empty; the socket's queue is not
    Killing,    // the client left; waiting for the child to be gone
    // fetch
    Receiving,
    Writing,
    Reading,
};

constexpr u32 kMaxArgs  = 64;
constexpr u32 kArgBytes = 4096;
constexpr u32 kChunk    = 16 * 1024;

struct Runcast {
    Stage       stage = Stage::Idle;
    // serve
    NetListener listener;
    NetSocket   sock;
    Process     child;
    Handle     lock;
    const char* argv[kMaxArgs] = {};
    u32         argc = 0;
    char        argbuf[kArgBytes] = {};
    bool        out_done = false, err_done = false;
    // fetch
    Array<u8>   got;
    char        file[512] = {};
    Handle      request;
    // what the loop did, for --verbose
    u64         ticks = 0, loop_wakes = 0;
};

const char* stage_name(Stage s) {
    switch (s) {
    case Stage::Idle: return "idle";
    case Stage::Listening: return "listening";
    case Stage::Streaming: return "streaming";
    case Stage::Draining: return "draining";
    case Stage::Killing: return "killing";
    case Stage::Receiving: return "receiving";
    case Stage::Writing: return "writing";
    case Stage::Reading: return "reading";
    }
    return "?";
}

void set_stage(Runcast& r, Stage s) {
    KV_INFO("runcast", "%s -> %s", stage_name(r.stage), stage_name(s));
    r.stage = s;
}

// Everything the run holds, let go of. Safe to call twice.
void release(Engine& e, Runcast& r) {
    platform_unwatch(e, &r);
    if (r.child.is_open()) r.child.close();
    r.sock.close();
    if (r.listener.is_open()) r.listener.close();
    if (!r.lock.is_null()) { io_unlock_file(r.lock); r.lock = Handle{}; }
    r.got.free();
    r.stage = Stage::Idle;
}

void finish(Engine& e, Runcast& r, int code) {
    print_flush();
    PlatformWaits pw = platform_waits(e);
    KV_INFO("runcast", "%llu ticks; the loop asked the waker %llu times; the loop waited %llu times (%llu woken, %llu timed)",
            (unsigned long long)r.ticks, (unsigned long long)r.loop_wakes, (unsigned long long)pw.waits, (unsigned long long)pw.woken,
            (unsigned long long)pw.timed);
    release(e, r);
    tool_finish(e, code);
}

// ---- serve ------------------------------------------------------------------------------

// Moves what the child has written into the socket's queue, as much as the queue has room
// for. Returns nothing: out_done and err_done say when a stream has ended.
void pump_child(Runcast& r) {
    u8 buf[kChunk];
    for (u32 pass = 0; pass < 8; ++pass) {
        bool any = false;
        if (!r.out_done && r.sock.room() >= kChunk) {
            i32 n = r.child.read_out(buf, sizeof buf);
            if (n > 0) { r.sock.write(buf, (u32)n); any = true; }
            if (n < 0) r.out_done = true;
        }
        if (!r.err_done && r.sock.room() >= kChunk) {
            i32 n = r.child.read_err(buf, sizeof buf);
            if (n > 0) { r.sock.write(buf, (u32)n); any = true; }
            if (n < 0) r.err_done = true;
        }
        if (!any) break;
    }
}

void step_serve(Engine& e, Runcast& r) {
    if (r.stage == Stage::Listening) {
        Handle h = r.listener.accept();
        if (h.is_null()) return;
        r.sock.adopt(h);
        r.listener.close(); // one client a run
        ProcessDesc desc;
        desc.argv = Span<const char* const>(r.argv, r.argc);
        if (!r.child.spawn(desc)) {
            print_err("runcast: cannot start '%s': %s\n", r.argv[0], r.child.error);
            finish(e, r, 1);
            return;
        }
        KV_INFO("runcast", "started '%s', pid %lld", r.argv[0], (long long)r.child.pid);
        set_stage(r, Stage::Streaming);
    }
    if (r.stage == Stage::Streaming) {
        u8 scratch[256];
        i32 n = r.sock.read(scratch, sizeof scratch); // nothing is expected; this is how a close is seen
        if (n < 0 || r.sock.closed) {
            KV_INFO("runcast", "the client left; killing the child");
            r.child.kill();
            set_stage(r, Stage::Killing);
        } else {
            pump_child(r);
            r.sock.flush();
            bool running = r.child.running();
            if (!running && r.out_done && r.err_done) set_stage(r, Stage::Draining);
        }
    }
    if (r.stage == Stage::Draining) {
        r.sock.flush();
        if (r.sock.pending() == 0 || r.sock.closed) {
            int code = r.child.code;
            finish(e, r, code);
        }
        return;
    }
    if (r.stage == Stage::Killing) {
        if (!r.child.running()) {
            int code = r.child.code;
            finish(e, r, code ? code : 1);
        }
    }
}

// ---- fetch ------------------------------------------------------------------------------

void step_fetch(Engine& e, Runcast& r) {
    IoQueue* io = platform_io(e);
    if (r.stage == Stage::Receiving) {
        u8 buf[kChunk];
        for (u32 pass = 0; pass < 16; ++pass) {
            i32 n = r.sock.read(buf, sizeof buf);
            if (n > 0) {
                memcpy(r.got.push_n((u32)n), buf, (usize)n);
                output_write(false, (const char*)buf, (usize)n); // a line that ends is flushed at the end of the tick
                continue;
            }
            if (n == 0) break;
            // Over. A connection that was refused is not a server that closed.
            if (r.sock.error) {
                print_err("runcast: %s\n", r.sock.link_state == NetLink::Failed ? "nobody is listening on that port: connection refused" : os_error_text(r.sock.error));
                finish(e, r, 1);
                return;
            }
            if (!r.file[0]) { finish(e, r, 0); return; }
            IoWriteRequest w;
            w.path = r.file;
            w.data = r.got.data;
            w.size = r.got.len;
            r.request = io->write_file(w);
            if (r.request.is_null()) {
                print_err("runcast: could not queue the write of '%s'\n", r.file);
                finish(e, r, 1);
                return;
            }
            set_stage(r, Stage::Writing);
            return;
        }
        r.sock.flush();
        return;
    }
    if (r.stage == Stage::Writing) {
        IoCompletion c;
        if (!io->take(r.request, &c)) return;
        if (c.status != IoStatus::Done || c.written != r.got.len) {
            print_err("runcast: writing '%s' failed: %s (%llu of %u bytes)\n", r.file, c.error ? os_error_text(c.error) : "short write",
                      (unsigned long long)c.written, r.got.len);
            finish(e, r, 1);
            return;
        }
        IoRequest q;
        q.path = r.file;
        r.request = io->read_file(q);
        if (r.request.is_null()) {
            print_err("runcast: could not queue the read of '%s'\n", r.file);
            finish(e, r, 1);
            return;
        }
        set_stage(r, Stage::Reading);
        return;
    }
    if (r.stage == Stage::Reading) {
        IoCompletion c;
        if (!io->take(r.request, &c)) return;
        int code = 0;
        if (c.status != IoStatus::Done) {
            print_err("runcast: reading '%s' back failed: %s\n", r.file, os_error_text(c.error));
            code = 1;
        } else if (c.bytes.size != r.got.len || (r.got.len && memcmp(c.bytes.data, r.got.data, r.got.len) != 0)) {
            print_err("runcast: '%s' read back differs from what was received (%llu bytes on disk, %u received)\n", r.file,
                      (unsigned long long)c.bytes.size, r.got.len);
            code = 1;
        } else {
            KV_INFO("runcast", "'%s' verified, %u bytes", r.file, r.got.len);
        }
        c.bytes.free();
        finish(e, r, code);
    }
}

// ---- the system and the waker -----------------------------------------------------------

bool waker(Engine& e, void* user);

void pump(TickContext& ctx) {
    Runcast& r = ctx.state_as<Runcast>();
    ++r.ticks;
    switch (r.stage) {
    case Stage::Idle: break;
    case Stage::Listening:
    case Stage::Streaming:
    case Stage::Draining:
    case Stage::Killing: step_serve(*ctx.engine, r); break;
    case Stage::Receiving:
    case Stage::Writing:
    case Stage::Reading: step_fetch(*ctx.engine, r); break;
    }
    // Nothing here waits or asks for the next tick: the waker below says what the run is
    // waiting on, and the tool's loop sleeps until one of those stirs.
}

// Says what the run is waiting on, so a loop that waits is woken by it.
bool waker(Engine& e, void* user) {
    Runcast&      r = *(Runcast*)user;
    ++r.loop_wakes;
    PlatformWatch w(e);
    switch (r.stage) {
    case Stage::Idle: break;
    case Stage::Listening: w.add(r.listener); break;
    case Stage::Streaming: w.add(r.sock); w.add(r.child); w.soon(); break; // a child's end is not a thing to watch: ask soon
    case Stage::Draining: w.add(r.sock); break;
    case Stage::Killing: w.soon(); break;
    case Stage::Receiving: w.add(r.sock); break;
    case Stage::Writing:
    case Stage::Reading: w.add(*platform_io(e)); break;
    }
    platform_watch(e, &r, w);
    return false;
}

// ---- commands ---------------------------------------------------------------------------

bool parse_port(CliContext& ctx, const char* arg, u16* out) {
    u32 port = 0;
    if (!cli_arg_u32(ctx, arg, "port", &port)) return false;
    if (port > 65535) { ctx.fail("port: %u is past 65535", port); return false; }
    *out = (u16)port;
    return true;
}

void cmd_serve(CliContext& ctx, Span<const char* const> args) {
    Engine&  e = *ctx.engine;
    Runcast* r = engine_module_state_as<Runcast>(e, "runcast");
    if (!r || r->stage != Stage::Idle) { ctx.fail("runcast is busy"); return; }
    if (args.len < 3 || strcmp(args[1], "--") != 0) { ctx.fail("runcast serve PORT -- CMD ARGS..."); return; }
    u16 port = 0;
    if (!parse_port(ctx, args[0], &port)) return;

    // The command, kept: the words are the request's and go when this returns.
    u32 used = 0;
    r->argc = 0;
    for (u32 i = 2; i < args.len; ++i) {
        usize n = strlen(args[i]) + 1;
        if (r->argc == kMaxArgs || used + n > kArgBytes) { ctx.fail("the command is too long"); return; }
        memcpy(r->argbuf + used, args[i], n);
        r->argv[r->argc++] = r->argbuf + used;
        used += (u32)n;
    }

    // One server per PORT argument.
    const char* dir = platform_user_dir(e, UserDir::Cache, "korvid", "runcast");
    char        path[KV_PLATFORM_PATH_MAX];
    char        who[64], holder[128] = {};
    format(path, sizeof path, "%sserve-%u.lock", dir, (u32)port);
    format(who, sizeof who, "runcast serve %u", (u32)port);
    i32 err = 0;
    if (!io_lock_file(path, who, &r->lock, holder, sizeof holder, &err)) {
        if (err == EWOULDBLOCK) ctx.fail("port argument %u is being served already, by '%s' (%s)", (u32)port, holder, path);
        else ctx.fail("cannot take the lock '%s': %s", path, os_error_text(err));
        return;
    }

    err = 0;
    if (!r->listener.open(port, NetScope::Loopback, &err)) {
        ctx.fail("cannot listen on port %u: %s", (u32)port, os_error_text(err));
        io_unlock_file(r->lock);
        r->lock = Handle{};
        return;
    }
    r->out_done = r->err_done = false;
    ctx.print("port %u", (u32)r->listener.port);
    print_flush();
    set_stage(*r, Stage::Listening);
}

void cmd_fetch(CliContext& ctx, Span<const char* const> args) {
    Engine&  e = *ctx.engine;
    Runcast* r = engine_module_state_as<Runcast>(e, "runcast");
    if (!r || r->stage != Stage::Idle) { ctx.fail("runcast is busy"); return; }
    if (args.len < 1 || args.len > 2) { ctx.fail("runcast fetch PORT [FILE]"); return; }
    u16 port = 0;
    if (!parse_port(ctx, args[0], &port)) return;
    r->file[0] = 0;
    if (args.len == 2) {
        i32 n = format(r->file, sizeof r->file, "%s", args[1]);
        if (n < 0 || (usize)n >= sizeof r->file) { ctx.fail("the file's path is too long"); return; }
    }
    i32 err = 0;
    if (!net_connect_local(port, &r->sock, &err)) {
        ctx.fail("cannot connect to port %u: %s", (u32)port, os_error_text(err));
        return;
    }
    os_stdio_binary(); // what the child wrote, byte for byte
    r->got.clear();
    set_stage(*r, Stage::Receiving);
}

constinit const CliCommandDesc k_commands[] = {
    {.name = "runcast serve", .help = "PORT -- CMD ARGS...: run CMD for the client that connects, its output down the socket", .fn = cmd_serve},
    {.name = "runcast fetch", .help = "PORT [FILE]: print what the server sends; with FILE, write it and check it", .fn = cmd_fetch},
};

constinit const SystemDesc k_systems[] = {
    {.name = ".pump", .phase = Phase::PreSim, .fn = pump, .main_thread = true, .runs = RunWhen::Always},
};

bool runcast_startup(Engine& e, ModuleInstance& self) {
    Runcast* r = arena_push<Runcast>(self.arena);
    if (!r) return false;
    self.set_state(r);
    r->got.alloc      = &self.general;
    r->sock.out.alloc = &self.general;
    return engine_add_watched_waker(e, waker, r);
}

void runcast_shutdown(Engine& e, ModuleInstance& self) {
    Runcast* r = self.state_as<Runcast>();
    if (!r) return;
    engine_remove_waker(e, waker, r);
    release(e, *r);
}

} // namespace

KV_MODULE_DEFINE(runcast_module) = {
    .name     = "runcast",
    .systems  = k_systems,
    .commands = k_commands,
    .startup  = runcast_startup,
    .shutdown = runcast_shutdown,
    .memory   = {.reserve = 64 * 1024},
};

} // namespace kv
