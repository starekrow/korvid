#include "cli_internal.h"
#include "korvid/core/engine.h"
#include "korvid/core/os_error.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/utf8.h"
#include "korvid/platform/module.h"
#include "korvid/platform/stdin.h"
#include <string.h>

namespace kv {

namespace {

CliConfig g_config;

} // namespace

usize cli_ascii_escape(const char* in, char* out, usize cap) {
    usize at = 0;
    auto put = [&](const char* s, usize n) {
        if (at + n < cap) memcpy(out + at, s, n);
        else if (at < cap) cap = at + 1; // the first escape that does not fit ends the output
        at += n;
    };
    const char* p = in;
    while (*p) {
        const char* start = p;
        u32 cp = utf8_next(p);
        if (cp == '\\') put("\\\\", 2);
        else if (cp < 0x80) put(start, 1);
        else { char e[16]; int n = format(e, sizeof e, "\\u{%X}", cp); put(e, (usize)n); }
    }
    if (cap) out[at < cap ? at : cap - 1] = 0;
    return at;
}

usize cli_ascii_unescape(const char* in, char* out, usize cap) {
    usize at = 0;
    auto put = [&](const char* s, usize n) {
        if (at + n < cap) memcpy(out + at, s, n);
        else if (at < cap) cap = at + 1;
        at += n;
    };
    for (const char* p = in; *p;) {
        if (p[0] == '\\' && p[1] == '\\') { put("\\", 1); p += 2; continue; }
        if (p[0] == '\\' && p[1] == 'u' && p[2] == '{') {
            u32 cp = 0, digits = 0;
            const char* q = p + 3;
            for (; digits < 7; ++q, ++digits) {
                char c = *q;
                u32 d = c >= '0' && c <= '9' ? (u32)(c - '0') : c >= 'a' && c <= 'f' ? (u32)(c - 'a' + 10) : c >= 'A' && c <= 'F' ? (u32)(c - 'A' + 10) : 16;
                if (d == 16) break;
                cp = cp * 16 + d;
            }
            if (digits > 0 && digits <= 6 && *q == '}') {
                char e[4];
                put(e, utf8_encode(cp, e));
                p = q + 1;
                continue;
            }
        }
        put(p, 1);
        ++p;
    }
    if (cap) out[at < cap ? at : cap - 1] = 0;
    if (at >= cap && cap) utf8_trim(out); // raw UTF-8 passes through and may have been cut
    return at;
}

CliState* cli_state(Engine& e) { return engine_module_state_as<CliState>(e, "cli"); }

namespace {

CliState* state(Engine& e) { return cli_state(e); }

// ---- the protocol -------------------------------------------------------------------------------

struct Request {
    Session*  session; // null: an external sink
    void    (*sink)(void*, const char*);
    void*     user;
    char      id[32];  // "#id" or empty
    CliState* cli;
};

void respond(Request& rq, const char* line) {
    if (rq.sink) { rq.sink(rq.user, line); return; }
    if (!rq.session) return; // cli_run with no sink: the line is discarded
    Session& s = *rq.session;
    char out[kLineMax + 48];
    int n = rq.id[0] ? format(out, sizeof out, "%s %s\n", rq.id, line) : format(out, sizeof out, "%s\n", line);
    if (n < 0) return;
    const char* send = out;
    char esc[kLineMax * 4];
    if (s.ascii) { cli_ascii_escape(out, esc, sizeof esc); send = esc; n = (int)strlen(esc); }
    if (s.is_stdin) { print("%s", send); print_flush(); return; }
    if (s.dropped && s.sock.out.len == 0) { s.sock.write_str("!dropped\n"); s.dropped = false; }
    if (!s.sock.write(send, (u32)n)) { s.dropped = true; ++rq.cli->dropped; }
}

void request_sink(void* user, const char* line) { respond(*(Request*)user, line); }

// Splits a line into words; double quotes group words. Returns the count, at most kMaxArgs.
u32 split(char* line, const char** words) {
    u32 n = 0;
    char* p = line;
    while (*p && n < kMaxArgs) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        if (*p == '"') {
            ++p;
            words[n++] = p;
            while (*p && *p != '"') ++p;
            if (*p) *p++ = 0;
        } else {
            words[n++] = p;
            while (*p && *p != ' ' && *p != '\t') ++p;
            if (*p) *p++ = 0;
        }
    }
    return n;
}

// The command whose name has the most words matching the line's first words.
const CliCommandDesc* match(CliState& cli, const char** words, u32 count, u32* consumed) {
    const CliCommandDesc* best = nullptr;
    u32 best_words = 0;
    for (const CliCommandDesc* c : cli.commands) {
        const char* name = c->name;
        u32 w = 0;
        bool ok = true;
        while (ok && *name) {
            const char* sp = strchr(name, ' ');
            usize len = sp ? (usize)(sp - name) : strlen(name);
            if (w >= count || strncmp(words[w], name, len) != 0 || words[w][len] != 0) { ok = false; break; }
            ++w;
            name = sp ? sp + 1 : name + len;
        }
        if (ok && w > best_words) { best = c; best_words = w; }
    }
    *consumed = best_words;
    return best;
}

// Runs one request: parse, dispatch, frame the answer.
bool run_request(Engine& e, CliState& cli, Request& rq, const char* text) {
    char line[kLineMax];
    if (rq.session && rq.session->ascii) cli_ascii_unescape(text, line, sizeof line);
    else utf8_copy(line, sizeof line, text);
    const char* words[kMaxArgs];
    u32 n = split(line, words);
    rq.id[0] = 0;
    u32 first = 0;
    if (n && words[0][0] == '#') { strncpy(rq.id, words[0], sizeof rq.id - 1); first = 1; }
    ++cli.requests;

    CliContext ctx;
    ctx.engine = &e;
    ctx.sink   = request_sink;
    ctx.user   = &rq;
    ctx.cwd    = rq.session ? rq.session->cwd : nullptr;
    if (first >= n) {
        ctx.fail("empty request");
    } else {
        u32 consumed = 0;
        const CliCommandDesc* cmd = match(cli, words + first, n - first, &consumed);
        if (!cmd) ctx.fail("unknown command '%s%s%s'; try help", words[first], n - first > 1 ? " " : "", n - first > 1 ? words[first + 1] : "");
        else cmd->fn(ctx, Span<const char* const>(words + first + consumed, n - first - consumed));
    }
    if (ctx.failed) ++cli.failed;
    char tail[256];
    if (!rq.session) { // an external caller (cli_run): the status line is the result
        if (ctx.failed && rq.sink) { format(tail, sizeof tail, "err: %s", ctx.error); rq.sink(rq.user, tail); }
        return !ctx.failed;
    }
    if (rq.id[0]) format(tail, sizeof tail, ctx.failed ? "err %s" : "ok", ctx.error);
    else          format(tail, sizeof tail, ctx.failed ? "err: %s" : "ok", ctx.error);
    respond(rq, tail);
    return !ctx.failed;
}

// Feeds bytes into a session's line buffer and runs every complete line.
void feed(Engine& e, CliState& cli, Session& s, const char* data, u32 len) {
    for (u32 i = 0; i < len; ++i) {
        char c = data[i];
        if (c == '\n') {
            if (!s.overflow) {
                s.line[s.line_len] = 0;
                u32 l = s.line_len;
                while (l && (s.line[l - 1] == '\r' || s.line[l - 1] == ' ')) s.line[--l] = 0;
                if (l) {
                    Request rq{&s, nullptr, nullptr, {}, &cli};
                    run_request(e, cli, rq, s.line);
                }
            } else {
                Request rq{&s, nullptr, nullptr, {}, &cli};
                respond(rq, "err: line too long");
            }
            s.line_len = 0;
            s.overflow = false;
        } else if (s.line_len + 1 < kLineMax) {
            s.line[s.line_len++] = c;
        } else {
            s.overflow = true;
        }
    }
}

// ---- the commands every build has -------------------------------------------------------------

void cli_help(CliContext& ctx, Span<const char* const>) {
    CliState* cli = state(*ctx.engine);
    for (const CliCommandDesc* c : cli->commands) ctx.print("%-16s %s", c->name, c->help ? c->help : "");
    char line[512];
    usize at = 0;
    for (const CliMountDesc* m : cli->mounts) at += (usize)format(line + at, sizeof line - at, "%s%s", at ? " " : "", m->path);
    ctx.print("the filesystem: ls, cd, pwd, print, patch over %s", line);
}

void cli_quit(CliContext& ctx, Span<const char* const>) {
    ctx.print("quitting");
    engine_request_quit(*ctx.engine);
}

// Sim control. The change applies from the next tick; the commands run inside a tick.
void cli_pause(CliContext& ctx, Span<const char* const>) {
    engine_pause(*ctx.engine, true);
    ctx.print("paused at tick %llu (holds %#x)", (unsigned long long)ctx.engine->clock.tick_count, ctx.engine->clock.holds);
}

void cli_resume(CliContext& ctx, Span<const char* const>) {
    engine_pause(*ctx.engine, false);
    ctx.engine->clock.step_ticks = 0;
    if (ctx.engine->clock.holds) ctx.print("still held at tick %llu (holds %#x): resume ends only the user hold", (unsigned long long)ctx.engine->clock.tick_count, ctx.engine->clock.holds);
    else ctx.print("running from tick %llu", (unsigned long long)ctx.engine->clock.tick_count);
}

void cli_step(CliContext& ctx, Span<const char* const> args) {
    u32 n = 1;
    if (args.len && !cli_arg_u32(ctx, args[0], "ticks", &n)) return;
    engine_step(*ctx.engine, n);
    ctx.print("stepping %u tick%s from %llu", n, n == 1 ? "" : "s", (unsigned long long)ctx.engine->clock.tick_count);
}

void cli_rate(CliContext& ctx, Span<const char* const> args) {
    if (!args.len) { ctx.print("rate %.3f%s", ctx.engine->clock.rate, ctx.engine->clock.paused ? " (paused)" : ""); return; }
    f32 r = 0.0f;
    if (!cli_arg_f32(ctx, args[0], "rate", &r)) return;
    if (!engine_rate(*ctx.engine, (f64)r)) { ctx.fail("rate must be positive; use pause"); return; }
    ctx.print("rate %.3f", (f64)r);
}

void cli_time_scale(CliContext& ctx, Span<const char* const> args) {
    if (!args.len) { ctx.print("time scale %.3f (rate %.3f)", ctx.engine->clock.scale, ctx.engine->clock.rate); return; }
    f32 s = 0.0f;
    if (!cli_arg_f32(ctx, args[0], "scale", &s)) return;
    if (!engine_time_scale(*ctx.engine, s)) { ctx.fail("time scale must be zero or more"); return; }
    ctx.print("time scale %.3f", (f64)s);
}

constinit const CliCommandDesc k_commands[] = {
    {.name = "help",   .help = "list the commands", .fn = cli_help},
    {.name = "quit",   .help = "ask the engine to quit", .fn = cli_quit},
    {.name = "pause",  .help = "hold the sim; menus, input and the extract keep running", .fn = cli_pause},
    {.name = "resume", .help = "let the sim run again", .fn = cli_resume},
    {.name = "step",   .help = "[N] let N sim ticks (1) through the pause", .fn = cli_step},
    {.name = "rate",   .help = "[X] show or set the developer's rate while unpaused", .fn = cli_rate},
    {.name = "time scale", .help = "[X] show or set the game's time scale (times the rate; 0 runs no sim ticks)", .fn = cli_time_scale},
    {.name = "ls",     .help = "[-n N] [path]: what is under a directory of the filesystem (N entries at most, 200 by default)", .fn = cli_fs_ls},
    {.name = "cd",     .help = "[path]: change the session's working directory (/ by default)", .fn = cli_fs_cd},
    {.name = "pwd",    .help = "the working directory", .fn = cli_fs_pwd},
    {.name = "print",  .help = "<path>...: describe a node", .fn = cli_fs_print},
    {.name = "patch",  .help = "<path> <value>...: set a node", .fn = cli_fs_patch},
};

// ---- the module ------------------------------------------------------------------------------------

// The listener, the sessions and stdin, for an idle loop to wait on (KR26): what arrives
// wakes it, and the tick after reads it here.
void watch_sockets(Engine& e, CliState& cli) {
    PlatformWatch w(e);
    w.add(cli.listener);
    for (u32 i = 0; i < cli.session_count; ++i) {
        const Session& s = cli.sessions[i];
        if (s.is_stdin) { if (!s.stdin_eof) w.add(io_stdin_handle()); }
        else w.add(s.sock.fd); // what it reads; what it writes is flushed by the tick, not waited for
    }
    platform_watch(e, &cli, w);
}

bool cli_startup(Engine& e, ModuleInstance& self) {
    CliState* s = arena_push<CliState>(self.arena);
    if (!s) return false;
    self.set_state(s);
    s->config = g_config;
    s->commands.alloc = s->mounts.alloc = &self.general;
    for (const ModuleInstance& m : e.modules) {
        for (const CliCommandDesc& c : m.desc->commands) s->commands.push(&c);
        for (const CliMountDesc& mt : m.desc->mounts) s->mounts.push(&mt);
    }
    for (Session& sess : s->sessions) sess.sock.out.alloc = &self.general;

    bool use_stdin = !s->config.no_stdin && (s->config.stdin_ || !io_stdin_is_terminal());
    if (use_stdin) {
        Session& sess = s->sessions[s->session_count++];
        sess.is_stdin = true;
        sess.ascii    = s->config.ascii;
    }
    if (s->config.port) {
        i32 err = 0;
        if (!s->listener.open(s->config.port, s->config.local_only ? NetScope::Loopback : NetScope::Any, &err)) {
            KV_ERROR("cli", "cannot listen on port %u: %s", s->config.port, os_error_text(err));
            return false;
        }
        KV_INFO("cli", "listening on %s:%u", s->config.local_only ? "127.0.0.1" : "0.0.0.0", s->listener.port);
    }
    watch_sockets(e, *s);
    KV_INFO("cli", "%u commands, %u mounts; stdin %s%s", s->commands.len, s->mounts.len, use_stdin ? "on" : "off", s->config.ascii ? "; ASCII transports" : "");
    return true;
}

void cli_shutdown(Engine& e, ModuleInstance& self) {
    CliState* s = self.state_as<CliState>();
    if (!s) return;
    platform_unwatch(e, s);
    for (Session& sess : s->sessions) { sess.sock.flush(); sess.sock.close(); sess.sock.out.free(); }
    s->listener.close();
    KV_INFO("cli", "%llu requests, %llu failed, %llu lines dropped", (unsigned long long)s->requests,
            (unsigned long long)s->failed, (unsigned long long)s->dropped);
    s->commands.free();
    s->mounts.free();
}

// Phase Input, main thread: accept, read, run, flush.
void cli_pump(TickContext& ctx) {
    CliState& cli = ctx.state_as<CliState>();
    Engine& e = *ctx.engine;
    for (Handle fd; cli.listener.is_open() && os_handle_open(fd = cli.listener.accept());) { // no --cli-port: no listener
        if (cli.session_count >= kMaxSessions) { net_close(fd); KV_WARN("cli", "connection refused: %u sessions", kMaxSessions); continue; }
        Session& s = cli.sessions[cli.session_count++];
        s = Session{};
        s.sock.out.alloc = cli.commands.alloc;
        s.sock.adopt(fd);
        s.ascii = cli.config.ascii;
        KV_INFO("cli", "session %u connected", cli.session_count - 1);
    }
    char buf[4096];
    for (u32 i = 0; i < cli.session_count; ++i) {
        Session& s = cli.sessions[i];
        if (s.is_stdin) {
            if (s.stdin_eof) continue;
            for (;;) {
                i32 n = io_stdin_read(buf, sizeof buf);
                if (n > 0) { feed(e, cli, s, buf, (u32)n); continue; }
                if (n < 0) s.stdin_eof = true; // end of stream, or an error: stdin is done
                break;
            }
            continue;
        }
        for (;;) {
            i32 n = s.sock.read(buf, sizeof buf);
            if (n > 0) { feed(e, cli, s, buf, (u32)n); continue; }
            if (n < 0) { KV_INFO("cli", "session %u closed", i); }
            break;
        }
        s.sock.flush();
    }
    // Drop closed sessions, keeping order.
    u32 kept = 0;
    for (u32 i = 0; i < cli.session_count; ++i) {
        Session& s = cli.sessions[i];
        if (!s.is_stdin && s.sock.closed) { s.sock.close(); s.sock.out.free(); continue; }
        if (kept != i) { cli.sessions[kept] = s; cli.sessions[i] = Session{}; cli.sessions[i].sock.out.alloc = cli.commands.alloc; }
        ++kept;
    }
    cli.session_count = kept;
    watch_sockets(e, cli);
}

constinit const SystemDesc k_systems[] = {
    {.name = ".pump", .phase = Phase::Input, .fn = cli_pump, .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(cli_module) = {
    .name     = "cli",
    .systems  = k_systems,
    .commands = k_commands,
    .mounts   = k_engine_mounts,
    .startup  = cli_startup,
    .shutdown = cli_shutdown,
    .memory   = {.reserve = 256 * 1024},
};

void cli_configure(const CliConfig& config) { g_config = config; }

bool cli_run(Engine& e, const char* line, void (*sink)(void* user, const char* line), void* user) {
    CliState* cli = state(e);
    if (!cli) return false;
    Request rq{nullptr, sink, user, {}, cli};
    return run_request(e, *cli, rq, line);
}

CliStats cli_stats(Engine& e) {
    CliState* cli = state(e);
    CliStats st{};
    if (cli) { st.sessions = cli->session_count; st.requests = cli->requests; st.failed = cli->failed; st.dropped = cli->dropped; }
    return st;
}

} // namespace kv
