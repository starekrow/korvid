#include "korvid/tool/tool.h"
#include "korvid/core/format.h"
#include "korvid/tool/module.h"
#include "korvid/cli/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/profile.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "korvid/core/time.h"
#include "korvid/core/os_process.h"

namespace kv {

namespace {

struct ToolState {
    bool running  = false; // a tool_run is driving this engine
    bool finished = false;
    int  code     = 0;
};

ToolState* state(Engine& e) { return engine_module_state_as<ToolState>(e, "tool"); }

bool tool_startup(Engine&, ModuleInstance& self) {
    ToolState* s = arena_push<ToolState>(self.arena);
    if (!s) return false;
    self.set_state(s);
    return true;
}

// The request's lines are the tool's output; the failure line goes to stderr.
void print_line(void* user, const char* line) {
    bool* failed = (bool*)user;
    if (strncmp(line, "err: ", 5) == 0) { *failed = true; print_err("%s\n", line + 5); return; }
    print("%s\n", line);
}

void usage(const char* name) {
    print_err("usage: %s [--threads N] [--verbose] [--max-ticks N] [--cli-port N] <command> [args...]\n"
                    "       %s help    (every command this tool knows)\n", name, name);
}

// --prof-report: every zone over the whole run, by wall time. On stderr, whatever the log
// level, as a tool's own output is on stdout.
void report_profile(const char* name) {
    ProfStat stats[48];
    u32 frames = 0;
    u32 n = prof_top(Span<ProfStat>(stats, KV_COUNTOF(stats)), &frames);
    print_err("%s: profile over %u ticks, summed over threads\n", name, frames);
    print_err("  %-32s %10s %10s %8s %10s %10s\n", "zone", "total ms", "cpu ms", "calls", "mean us", "worst ms");
    for (u32 i = 0; i < n; ++i) {
        const ProfStat& z = stats[i];
        f64 total = prof_ms(z.wall);
        print_err("  %-32s %10.1f %10.1f %8u %10.1f %10.2f%s\n", z.name, total, prof_ms(z.cpu), z.calls,
                  z.calls ? total * 1000.0 / z.calls : 0.0, prof_ms(z.worst), z.wait ? "  (wait)" : "");
    }
}

} // namespace

KV_MODULE_DEFINE(tool_module) = {
    .name    = "tool",
    .startup = tool_startup,
    .memory  = {.reserve = 4096},
};

void tool_finish(Engine& e, int code) {
    ToolState* s = state(e);
    if (!s || s->finished) return;
    s->finished = true;
    s->code     = code;
}

bool tool_running(Engine& e) {
    ToolState* s = state(e);
    return s && s->running && !s->finished;
}

// What a tool gives its Work cycle between two ticks: enough for a slice of real work, little
// enough that a tool's tick systems keep their pace.
constexpr f64 k_work_slice = 0.005;
// How long an idle tool goes between ticks nobody asked for: the frame loop's heartbeat.
constexpr f64 k_heartbeat = 0.25;

int tool_run(int argc, char** argv, Span<const ModuleDesc* const> modules, const ToolConfig& config) {
    // A tool's output is read by programs (tests, scripts, another tool) as often as by
    // people: the bytes it writes, `\n` and all, the same on every platform.
    os_stdio_binary();
    u32 threads = 0;
    u64 max_ticks = 0;
    bool verbose = false;
    u32 cli_port = 0;
    const char* trace_path = nullptr; // --profile FILE: a Chrome trace of the run
    bool prof_report = false;          // --prof-report: every zone's total over the run, at the end
    int first = 1;
    for (; first < argc && strncmp(argv[first], "--", 2) == 0; ++first) {
        if (strcmp(argv[first], "--threads") == 0 && first + 1 < argc) threads = (u32)atoi(argv[++first]);
        else if (strcmp(argv[first], "--max-ticks") == 0 && first + 1 < argc) max_ticks = (u64)atoll(argv[++first]);
        else if (strcmp(argv[first], "--verbose") == 0) verbose = true;
        else if (strcmp(argv[first], "--cli-port") == 0 && first + 1 < argc) cli_port = (u32)atoi(argv[++first]);
        else if (strcmp(argv[first], "--profile") == 0 && first + 1 < argc) trace_path = argv[++first];
        else if (strcmp(argv[first], "--prof-report") == 0) prof_report = true;
        else { print_err("%s: unknown option %s\n", config.name, argv[first]); usage(config.name); return 2; }
    }
    if (first >= argc) { usage(config.name); return 2; }

    // One request: the tool's name, then the words, quoted where they hold spaces. "help"
    // alone is the CLI's own.
    char line[2048];
    usize at = 0;
    bool help = argc - first == 1 && strcmp(argv[first], "help") == 0;
    if (!help) at += (usize)format(line, sizeof line, "%s", config.name);
    for (int i = first; i < argc && at < sizeof line; ++i) {
        if (strchr(argv[i], '"')) { print_err("%s: arguments may not contain '\"'\n", config.name); return 2; }
        bool quote = strchr(argv[i], ' ') || strchr(argv[i], '\t') || !argv[i][0];
        at += (usize)format(line + at, sizeof line - at, "%s%s%s%s", at ? " " : "", quote ? "\"" : "", argv[i], quote ? "\"" : "");
    }
    if (at >= sizeof line) { print_err("%s: command line too long\n", config.name); return 2; }

    LogLevel was = log_level();
    if (!verbose && was < LogLevel::Warn) log_set_level(LogLevel::Warn);
    CliConfig cli;
    cli.no_stdin = true; // a tool's stdin is the build's, not a CLI session
    if (config.session && strcmp(argv[first], config.session) == 0) { cli.no_stdin = false; cli.stdin_ = true; }
    cli.port = (u16)cli_port;
    cli_configure(cli);
    PlatformMounts mounts;
    mounts.none = true; // a tool mounts what its commands name, never the assets beside it
    platform_configure_mounts(mounts);
    // The profiler, as the frame loop has it, a tick for a frame: on when something will
    // read it. The report is the run's totals, since a tool's ticks are not alike.
    bool profiling = trace_path || prof_report;
    if (profiling) {
        if (!KV_PROFILE) print_err("%s: the profiler exists in dev builds only\n", config.name);
        prof_enable(true);
        prof_cpu_time(prof_report); // CPU beside wall: what a zone computed against what it waited for
        prof_thread_name("main");
        if (trace_path) prof_trace_open(trace_path);
    }
    Engine e;
    e.threads = threads;
    int code;
    if (!engine_init(e, modules, config.engine)) {
        code = 1;
    } else {
        ToolState* s = state(e);
        if (!s) { KV_ERROR("tool", "the tool module is not in the list"); engine_shutdown(e); log_set_level(was); return 1; }
        s->running = true;
        bool failed = false;
        bool ran;
        {
            KV_ZONE("tool command"); // what the command does before its first tick
            ran = cli_run(e, line, print_line, &failed);
        }
        if (profiling) prof_collect();
        if (!ran || failed) {
            code = 2;
        } else if (help) {
            code = 0; // the CLI's own command: it printed, there is nothing to wait for
        } else {
            // The work runs in the tool's systems, over as many ticks as it takes. A tick
            // runs when something asks for one, and the loop sleeps when nothing does: the
            // same contract a game's modules have with the frame loop, so a tool that waits
            // on a socket or a child uses no CPU while it waits, once its module names them
            // in a watched waker's PlatformWatch.
            // What asks:
            //   - the platform's queue having requests on their way or completions to take;
            //   - Work pending (docs/design/cycles.md);
            //   - a module: engine_want_ticks, each tick it wants the next;
            //   - a waker (engine_add_watched_waker) saying something came, the time it
            //     asked for (PlatformWatch::at, soon) arriving, or what it watches stirring;
            //   - the heartbeat, k_heartbeat after the last tick, so a module that asked
            //     for nothing still runs, slowly enough to be noticed.
            // --max-ticks is a bounded run for a test: it ticks without waiting.
            const f64 dt = e.clock.tick_dt;
            u64 ticks = 0;
            IoQueue* io = platform_io(e);
            bool woke = true; // the first pass ticks: the command has just started its work
            f64 last_tick = time_now();
            while (!s->finished) {
                if (profiling) prof_frame(ticks);
                platform_pump(e);
                f64 now = time_now();
                bool io_busy = io && (io->pending() || io->completions().len);
                bool tick = woke || max_ticks || e.ticks_wanted || io_busy || engine_work_pending(e) || now - last_tick >= k_heartbeat;
                if (engine_wake(e)) tick = true; // the wakers: what came, and what each waits on next
                if (!tick) {
                    f64 wait = platform_idle_time(e, k_heartbeat - (now - last_tick));
                    if (wait > 0.0) platform_wait(e, wait);
                    woke = true; // whatever ended the wait, a tick looks at it
                    continue;
                }
                woke = false;
                last_tick = now;
                engine_frame(e, dt);
                engine_work(e, k_work_slice); // the Work cycle between ticks
                if (profiling) prof_collect();
                ++ticks;
                if (max_ticks && ticks >= max_ticks) { KV_ERROR("tool", "not finished after %llu ticks", (unsigned long long)ticks); break; }
                // A tick that only waits on the disk yields the core briefly.
                if (io && io->pending() && io->completions().len == 0 && !engine_work_pending(e)) time_sleep(0.0001);
            }
            print_flush_lines();
            code = s->finished ? s->code : 2;
        }
        s->running = false;
        engine_shutdown(e);
    }
    if (profiling) {
        prof_collect();
        if (prof_report) report_profile(config.name);
        prof_shutdown(); // the trace closed, the rings released before the leak check
    }
    log_set_level(was);
    return code;
}

int tool_main(int argc, char** argv, Span<const ModuleDesc* const> modules, const ToolConfig& config) {
    int code = tool_run(argc, argv, modules, config);
    names_shutdown();
    u64 live = mem_live_allocations();
    if (live && (KV_DEV || KV_DEBUG)) {
        KV_ERROR("tool", "%llu allocations still live at exit", (unsigned long long)live);
        if (code == 0) code = 3;
    }
    mem_shutdown();
    return code;
}

} // namespace kv
