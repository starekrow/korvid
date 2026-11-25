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
// calls tool_finish itself. The loop ticks when something asks for a tick (the I/O queue
// busy, Work pending, engine_want_ticks, a waker's PlatformWatch stirring or its time
// coming) and sleeps in platform_wait when nothing does, a quarter-second heartbeat apart
// at the slowest: a tool that waits on a socket or a child uses no CPU while it waits, and
// a system that advances only by being ticked says engine_want_ticks. The same commands work in any engine that links the module,
// from its CLI.
//
// Exit status: the code given to tool_finish; 2 for a request the CLI refused (unknown
// command, bad arguments) or a run stopped by --max-ticks; 1 if the engine did not start;
// 3 if the run leaked memory in a dev or debug build (the leak report names it). A tool
// whose systems never call tool_finish runs until --max-ticks or forever: finishing is
// the tool's job.
//
// Engine flags come before the command: --threads N, --verbose (INFO lines; a tool logs
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

} // namespace kv
