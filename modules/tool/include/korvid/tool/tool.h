// Korvid tool: the engine as a tool framework. A tool executable is a module list like a
// game's (core, platform, cli, tool, and the tool's own module); its main is
//
//   int main(int argc, char** argv) { return kv::tool_main(argc, argv, kv_module_list(), {.name = "kpack"}); }
//
// tool_main starts the engine headless (no window, no renderer, no frame pacing), joins the
// command line into one CLI request prefixed by the tool's name ("kpack list game.kpak"),
// and runs it. The tool's module registered that command: it checks its arguments,
// starts the work and returns, as every command does; the work runs in the module's
// systems over as many ticks as it takes (I/O completes across pumps, jobs fan out), and
// a system calls tool_finish(e, code) when it is done. A command with nothing to wait for
// calls tool_finish itself; one whose work is easiest written top to bottom hands it to
// tool_task (below). The loop ticks when something asks for a tick (reads or writes on the
// I/O queue, Work pending, engine_want_ticks, a standing watch or a waker's PlatformWatch
// stirring or its time coming, a tool_task returning) and sleeps in platform_wait when
// nothing does, a quarter-second heartbeat apart at the slowest: a tool that waits on a
// socket or a child uses no CPU while it waits, once its module watches them
// (platform_watch_add), and a system that advances only by being ticked says
// engine_want_ticks. Nothing watches a handle no module named. modules/tool/README.md, "A
// tool that waits" and "Straight-line work", has both shapes. The same commands work in
// any engine that links the module, from its CLI.
//
// Exit status: the code given to tool_finish; 2 for a request the CLI refused (unknown
// command, bad arguments, ctx.fail: the run ends there, before any tick) or a run stopped
// by --max-ticks; 1 if the engine did not start;
// 3 if the run leaked memory in a dev or debug build (the leak report names it). A tool
// whose systems never call tool_finish runs until --max-ticks or forever: finishing is
// the tool's job. A failure the work finds later (a port in use) is print_err and
// tool_finish(e, 1), which keeps 2 for usage.
//
// Engine flags come before the command; the scan stops at the first word not starting
// with "--", so the command's own words (a "--", a "-n") reach it untouched: --threads N, --verbose (INFO lines; a tool logs
// warnings and errors only by default), --max-ticks N (stop with 2 if not finished).
// --cli-port N opens the CLI's listener on loopback for the length of the run.
// --profile FILE writes the engine's profiler's trace of the run (Chrome's JSON events, as
// the frame loop's), a tick a frame; --prof-report prints every zone's total over the run,
// wall and CPU, on stderr at the end. Dev builds only: the profiler is compiled out of ship.
//
// A tool with a command that stays up (goldberg watch) names it in ToolConfig::session:
// while that command runs, stdin is a CLI session too, terminal or not, and the CLI's
// `quit` is the tool's to answer (Engine::quit_requested).
#pragma once
#include "korvid/core/atomic.h"
#include "korvid/core/engine.h"

namespace kv {

struct ToolConfig {
    const char*  name   = "tool";     // the command prefix and the name in usage lines
    const char*  session = nullptr;   // the first word of a command that runs as a session, or null
    EngineConfig engine = {};
};

// The whole program: options, engine, request, ticks, shutdown, leak check. Returns the
// exit status.
int  tool_main(int argc, char** argv, Span<const ModuleDesc* const> modules, const ToolConfig& config);
// The same without the process-wide shutdown (names, memory) after it, for tests.
int  tool_run(int argc, char** argv, Span<const ModuleDesc* const> modules, const ToolConfig& config);
// The work is done: the run stops after this tick with `code`. The first call stands.
void tool_finish(Engine& e, int code);
// True while a tool run has not finished (and in an engine with no tool module running,
// false): for commands that work both in a tool and in a game's CLI.
bool tool_running(Engine& e);

// ---- straight-line work ---------------------------------------------------------------
// Runs fn(arg, ctl) on a thread of its own, where blocking is fine (NetSocket::write_now,
// Process::wait, io_read_file, a loop that sleeps), while the engine goes on ticking: its
// systems and timers, the CLI and --cli-port, the I/O queue. When fn returns, the run
// finishes with what it returned as the exit status, as tool_finish(e, code) would (the
// first finish stands). For a tool whose work is easiest written as one function from top
// to bottom; modules/tool/README.md, "Straight-line work", has the shape.
//
// fn touches only `arg`, which stays where it is until the run ends, and what any thread
// may call: the platform's blocking calls and objects it owns, print and print_err (each
// call whole), the log. Not the engine, its ktables or another module's state. When the run
// ends otherwise (another finish, --max-ticks, a quit), ctl.cancelled() turns true, and
// the run waits for fn to return before it shuts down: a loop in fn looks at it, and a
// blocking call in fn has a timeout (write_now's seconds, Process::wait's).
//
// From a command or a system while a tool run is driving the engine (tool_running): false
// otherwise, and with eight running, or when no thread can be had.
struct ToolTaskCtl {
    u32  flag = 0; // set by the tool when the run is ending: atomic
    bool cancelled() const { return atomic_load(&flag, MO_RELAXED) != 0; }
};
typedef i32 (*ToolTaskFn)(void* arg, const ToolTaskCtl& ctl);
bool tool_task(Engine& e, ToolTaskFn fn, void* arg, const char* name = "task");

} // namespace kv
