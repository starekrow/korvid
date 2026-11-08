#include "korvid/cli/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/platform/net.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace kv {

namespace {

CliConfig g_config;

constexpr u32 kLineMax   = 1024;
constexpr u32 kMaxArgs   = 32;
constexpr u32 kMaxSessions = 16;

// One connection, or stdin.
struct Session {
    NetSocket sock;          // closed for the stdin session
    bool      is_stdin = false;
    bool      stdin_eof = false;
    char      line[kLineMax];
    u32       line_len = 0;
    bool      overflow = false; // the current line passed kLineMax: dropped at its end
    bool      dropped  = false; // output was dropped: say so once it drains
};

struct CliState {
    CliConfig   config;
    NetListener listener;
    Session     sessions[kMaxSessions];
    u32         session_count = 0;
    Array<const CliCommandDesc*> commands; // every module's, in module order
    u64         requests = 0, failed = 0, dropped = 0;
};

CliState* state(Engine& e) { return engine_module_state_as<CliState>(e, "cli"); }

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
    Session& s = *rq.session;
    char out[kLineMax + 48];
    int n = rq.id[0] ? snprintf(out, sizeof out, "%s %s\n", rq.id, line) : snprintf(out, sizeof out, "%s\n", line);
    if (n < 0) return;
    if (s.is_stdin) { fputs(out, stdout); fflush(stdout); return; }
    if (s.dropped && s.sock.out.len == 0) { s.sock.write_str("!dropped\n"); s.dropped = false; }
    if (!s.sock.write(out, (u32)n)) { s.dropped = true; ++rq.cli->dropped; }
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
    strncpy(line, text, sizeof line - 1);
    line[sizeof line - 1] = 0;
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
    if (first >= n) {
        ctx.fail("empty request");
    } else {
        u32 consumed = 0;
        const CliCommandDesc* cmd = match(cli, words + first, n - first, &consumed);
        if (!cmd) ctx.fail("unknown command '%s'; try help", words[first]);
        else cmd->fn(ctx, Span<const char* const>(words + first + consumed, n - first - consumed));
    }
    if (ctx.failed) ++cli.failed;
    char tail[256];
    if (rq.sink && !rq.session) { // an external caller: the status line is the result
        if (ctx.failed) { snprintf(tail, sizeof tail, "err: %s", ctx.error); rq.sink(rq.user, tail); }
        return !ctx.failed;
    }
    if (rq.id[0]) snprintf(tail, sizeof tail, ctx.failed ? "err %s" : "ok", ctx.error);
    else          snprintf(tail, sizeof tail, ctx.failed ? "err: %s" : "ok", ctx.error);
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
}

void cli_quit(CliContext& ctx, Span<const char* const>) {
    ctx.print("quitting");
    engine_request_quit(*ctx.engine);
}

// Sim control. The change applies from the next tick; the commands run inside a tick.
void cli_pause(CliContext& ctx, Span<const char* const>) {
    engine_pause(*ctx.engine, true);
    ctx.print("paused at tick %llu", (unsigned long long)ctx.engine->clock.tick_count);
}

void cli_resume(CliContext& ctx, Span<const char* const>) {
    engine_pause(*ctx.engine, false);
    ctx.engine->clock.step_ticks = 0;
    ctx.print("running from tick %llu", (unsigned long long)ctx.engine->clock.tick_count);
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

constinit const CliCommandDesc k_commands[] = {
    {.name = "help",   .help = "list the commands", .fn = cli_help},
    {.name = "quit",   .help = "ask the engine to quit", .fn = cli_quit},
    {.name = "pause",  .help = "hold the sim; menus, input and the extract keep running", .fn = cli_pause},
    {.name = "resume", .help = "let the sim run again", .fn = cli_resume},
    {.name = "step",   .help = "[N] let N sim ticks (1) through the pause", .fn = cli_step},
    {.name = "rate",   .help = "[X] show or set the time scale while unpaused", .fn = cli_rate},
};

// ---- the module ------------------------------------------------------------------------------------

bool cli_startup(Engine& e, ModuleInstance& self) {
    CliState* s = arena_push<CliState>(self.arena);
    if (!s) return false;
    self.set_state(s);
    s->config = g_config;
    s->commands.alloc = &self.general;
    for (const ModuleInstance& m : e.modules)
        for (const CliCommandDesc& c : m.desc->commands) s->commands.push(&c);
    for (Session& sess : s->sessions) sess.sock.out.alloc = &self.general;

    bool use_stdin = s->config.stdin_ || !isatty(STDIN_FILENO);
    if (use_stdin) {
        Session& sess = s->sessions[s->session_count++];
        sess.is_stdin = true;
    }
    if (s->config.port) {
        if (!s->listener.open(s->config.port, s->config.local_only)) return false;
        KV_INFO("cli", "listening on %s:%u", s->config.local_only ? "127.0.0.1" : "0.0.0.0", s->listener.port);
    }
    KV_INFO("cli", "%u commands; stdin %s", s->commands.len, use_stdin ? "on" : "off");
    return true;
}

void cli_shutdown(Engine&, ModuleInstance& self) {
    CliState* s = self.state_as<CliState>();
    if (!s) return;
    for (Session& sess : s->sessions) { sess.sock.flush(); sess.sock.close(); sess.sock.out.free(); }
    s->listener.close();
    KV_INFO("cli", "%llu requests, %llu failed, %llu lines dropped", (unsigned long long)s->requests,
            (unsigned long long)s->failed, (unsigned long long)s->dropped);
    s->commands.free();
}

// Phase Input, main thread: accept, read, run, flush.
void cli_pump(TickContext& ctx) {
    CliState& cli = ctx.state_as<CliState>();
    Engine& e = *ctx.engine;
    for (int fd; (fd = cli.listener.accept()) >= 0;) {
        if (cli.session_count >= kMaxSessions) { close(fd); KV_WARN("cli", "connection refused: %u sessions", kMaxSessions); continue; }
        Session& s = cli.sessions[cli.session_count++];
        s = Session{};
        s.sock.out.alloc = cli.commands.alloc;
        s.sock.adopt(fd);
        KV_INFO("cli", "session %u connected", cli.session_count - 1);
    }
    char buf[4096];
    for (u32 i = 0; i < cli.session_count; ++i) {
        Session& s = cli.sessions[i];
        if (s.is_stdin) {
            if (s.stdin_eof) continue;
            pollfd p{STDIN_FILENO, POLLIN, 0};
            while (poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLHUP))) {
                ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
                if (n > 0) { feed(e, cli, s, buf, (u32)n); continue; }
                if (n < 0 && errno == EINTR) continue;
                s.stdin_eof = true; // end of stream, or an error: stdin is done
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
}

constinit const SystemDesc k_systems[] = {
    {.name = "cli.pump", .phase = Phase::Input, .fn = cli_pump, .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(cli_module) = {
    .name     = "cli",
    .systems  = k_systems,
    .commands = k_commands,
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
