#include "korvid/tool/tool.h"
#include "korvid/tool/module.h"
#include "korvid/cli/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "korvid/core/time.h"

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
    if (strncmp(line, "err: ", 5) == 0) { *failed = true; fprintf(stderr, "%s\n", line + 5); return; }
    fputs(line, stdout);
    fputc('\n', stdout);
}

void usage(const char* name) {
    fprintf(stderr, "usage: %s [--threads N] [--verbose] [--max-ticks N] <command> [args...]\n"
                    "       %s help    (every command this tool knows)\n", name, name);
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

int tool_run(int argc, char** argv, Span<const ModuleDesc* const> modules, const ToolConfig& config) {
    u32 threads = 0;
    u64 max_ticks = 0;
    bool verbose = false;
    int first = 1;
    for (; first < argc && strncmp(argv[first], "--", 2) == 0; ++first) {
        if (strcmp(argv[first], "--threads") == 0 && first + 1 < argc) threads = (u32)atoi(argv[++first]);
        else if (strcmp(argv[first], "--max-ticks") == 0 && first + 1 < argc) max_ticks = (u64)atoll(argv[++first]);
        else if (strcmp(argv[first], "--verbose") == 0) verbose = true;
        else { fprintf(stderr, "%s: unknown option %s\n", config.name, argv[first]); usage(config.name); return 2; }
    }
    if (first >= argc) { usage(config.name); return 2; }

    // One request: the tool's name, then the words, quoted where they hold spaces. "help"
    // alone is the CLI's own.
    char line[2048];
    usize at = 0;
    bool help = argc - first == 1 && strcmp(argv[first], "help") == 0;
    if (!help) at += (usize)snprintf(line, sizeof line, "%s", config.name);
    for (int i = first; i < argc && at < sizeof line; ++i) {
        if (strchr(argv[i], '"')) { fprintf(stderr, "%s: arguments may not contain '\"'\n", config.name); return 2; }
        bool quote = strchr(argv[i], ' ') || strchr(argv[i], '\t') || !argv[i][0];
        at += (usize)snprintf(line + at, sizeof line - at, "%s%s%s%s", at ? " " : "", quote ? "\"" : "", argv[i], quote ? "\"" : "");
    }
    if (at >= sizeof line) { fprintf(stderr, "%s: command line too long\n", config.name); return 2; }

    LogLevel was = log_level();
    if (!verbose && was < LogLevel::Warn) log_set_level(LogLevel::Warn);
    CliConfig cli;
    cli.no_stdin = true; // a tool's stdin is the build's, not a CLI session
    cli_configure(cli);
    PlatformMounts mounts;
    mounts.none = true; // a tool mounts what its commands name, never the assets beside it
    platform_configure_mounts(mounts);
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
        if (!cli_run(e, line, print_line, &failed) || failed) {
            code = 2;
        } else if (help) {
            code = 0; // the CLI's own command: it printed, there is nothing to wait for
        } else {
            // The work runs in the tool's systems: tick until it says it is done, as fast as
            // the work allows; a tick that only waits on the disk yields the core briefly.
            const f64 dt = e.clock.tick_dt;
            u64 ticks = 0;
            IoQueue* io = platform_io(e);
            while (!s->finished) {
                platform_pump(e);
                engine_frame(e, dt);
                ++ticks;
                if (max_ticks && ticks >= max_ticks) { KV_ERROR("tool", "not finished after %llu ticks", (unsigned long long)ticks); break; }
                if (io && io->pending() && io->completions().len == 0) time_sleep(0.0001);
            }
            code = s->finished ? s->code : 2;
        }
        s->running = false;
        engine_shutdown(e);
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
