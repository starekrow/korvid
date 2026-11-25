# tool

The engine as a tool framework. A Korvid tool (the packer, a cooker, a converter) is an
engine executable like a game: a module list, modules with startup and shutdown, systems
with declared sets, the I/O queue, the job pool, the engine's allocators and its leak
report. What it does not have is a window, a renderer or frame pacing. This module is
the `main` such a tool needs and the rule for when it is done.

## How it works

`tool_main(argc, argv, modules, ToolConfig{.name = "kpack"})`:

1. Reads the engine's options from the front of the command line: `--threads N`,
   `--verbose` (a tool logs warnings and errors only unless asked), `--max-ticks N`.
   The scan stops at the first word that does not start with `--`, so everything from the
   command on reaches the command as it was typed, a `--` or a `-n` included. A word
   holding `"` is refused with exit status 2.
2. Joins the rest into one CLI request prefixed by the tool's name, quoting words with
   spaces: `kpack list build/game.kpak` becomes the request `kpack list build/game.kpak`.
   `kpack help` is the CLI's own `help`, which lists every command the tool knows.
3. Starts the engine headless with the module list. `platform` has no window and links
   no external library; a tool that wants a window or sound adds `platform_hid` or
   `platform_audio` to its deps, and kormake's host tool line says what it then links.
   The CLI never reads stdin in a tool: a build's pipes are not a session.
4. Runs the request. Its lines go to stdout. A failure (`ctx.fail`) goes to stderr and
   ends the run there, with exit status 2 and no tick run: it is for a request that is
   wrong as typed. A failure the work finds later (a port in use, a file missing) is the
   tool's to print with `print_err` and end with `tool_finish(e, 1)`, which keeps 2 for
   usage. stdout and stderr are binary on Windows too (`os_stdio_binary`): the bytes
   written are the bytes a reader gets, `\n` and all.
5. Ticks the engine when something asks for a tick, and sleeps when nothing does, until a
   system calls `tool_finish(e, code)` or `--max-ticks` is reached. The Work cycle
   (`engine_work`, [cycles](../../docs/concepts/cycles.md)) gets 5 ms after each tick; a
   tool's work may be in either cycle. What asks for a tick is what asks the frame loop:
   - the platform's I/O queue with requests on their way or completions to take;
   - Work pending;
   - a module calling `engine_want_ticks(e)`, each tick it wants the next;
   - a waker (`engine_add_watched_waker`) whose `PlatformWatch` stirs, or whose time
     comes ([platform](../platform/README.md), "What wakes it");
   - the heartbeat, a quarter of a second after the last tick.
   So a tool that works through the queue runs flat out with nothing to set up, and one
   that waits on a socket, a child process or a watched directory uses no CPU while it
   waits once its module says what it is waiting on: a watched waker and a
   `PlatformWatch` ([a tool that waits](#a-tool-that-waits), below). Nothing watches a
   handle the module has not named; without a waker such a tool hears its socket at the
   heartbeat's pace. A system that advances by being ticked, with none of the above
   pending, calls `engine_want_ticks`: without it, it runs at the heartbeat's pace.
   `--max-ticks` ticks without waiting.
6. Shuts the engine down. In debug and dev builds, memory still live after shutdown makes
   the exit status 3. kormake builds tools for the host in the host config (dev by
   default), so a tool a build runs makes the check too.

## Using it

**The tool's module** registers its verbs as commands, each named with the tool's prefix,
and its work as systems:

```cpp
void cmd_list(CliContext& ctx, Span<const char* const> args) {
    if (args.len != 1) { ctx.fail("kpack list <pack>"); return; }
    start_reading(*ctx.engine, args[0]);      // submit the read; the pump system takes it from here
}

void pump(TickContext& ctx) {
    ...
    if (done) tool_finish(*ctx.engine, failed ? 1 : 0);
}

constinit const CliCommandDesc k_commands[] = {{.name = "kpack list", .help = "kpack list <pack>: the entries", .fn = cmd_list}};
```

A command with nothing to wait for prints and calls `tool_finish` itself. A command
must not block: the work belongs in systems, so it can use the I/O queue and the job
pool like anything else in the engine.

A command that blocks anyway (a loop of its own, waiting on a socket or a child) runs
before the first tick, so nothing else runs while it does: no system, timer or handler,
no I/O queue completions, no Work, no `--cli-port`, and what it prints waits in stdout's
buffer until it calls `output_flush`. The engine does not object. That is enough for a
one-shot tool built only of blocking calls (`io_read_file`, `io_write_file`,
`NetSocket::write_now`, `Process::wait`), and nothing more. The smallest shape that does
not block is one system that does a step each tick and calls `engine_want_ticks` while it
has more to do; [a tool that waits](#a-tool-that-waits) is the same with a waker in place
of `engine_want_ticks`, so it sleeps when there is nothing to do.

**The executable** is a module with `output = "exe"` whose deps include `tool` and whose
`main` is:

```cpp
int main(int argc, char** argv) { return kv::tool_main(argc, argv, kv_module_list(), {.name = "kpack"}); }
```

### A tool that waits

A tool that serves a socket or runs a child has four pieces: a system that does the
I/O, a watched waker that says what the system is waiting on, the startup that adds the
waker and the shutdown that takes it away. For a tool that runs a command for whoever
connects and streams its output down the socket:

```cpp
// The work: whatever can be done now, then return. It never waits and never asks for a tick.
void pump(TickContext& ctx) { ... accept, read the child as far as sock.room() allows, flush ... }
constinit const SystemDesc k_systems[] = {
    {.name = ".pump", .phase = Phase::PreSim, .fn = pump, .main_thread = true, .runs = RunWhen::Always},
};

// Asked by the loop each pass: what the run is waiting on now.
bool waker(Engine& e, void* user) {
    Serve& s = *(Serve*)user;
    PlatformWatch w(e);
    if (listening) w.add(s.listener);
    if (streaming) { w.add(s.sock); w.add(s.child); w.soon(); }   // soon: the child's end is not watched
    platform_watch(e, &s, w);
    return false;                     // a stir gets the tick by itself; the reading is pump's
}

bool serve_startup(Engine& e, ModuleInstance& self) { ... return engine_add_watched_waker(e, waker, s); }
void serve_shutdown(Engine& e, ModuleInstance& self) {
    platform_unwatch(e, s);           // before what it watched is closed
    engine_remove_waker(e, waker, s);
    ... close the child, the socket, the listener ...
}
```

goldberg's `goldberg_wake` (`tools/goldberg/src/goldberg.cc`, with `goldberg_watch`) is a
waker in a real tool, watching its source directories, its listener, its programs'
sockets, a child process and its I/O queue; it does some of the work itself and returns
true when it found any. asset_live's (`modules/asset_live/src/module.cc`)
watches one socket.

- **The system is the tool's pump**: PreSim, main thread (sockets and children are the
  loop thread's), `RunWhen::Always`, as kbor's and goldberg's are.
- **Backpressure**: read from the child only as much as `sock.room()` says the socket
  will take, and leave the rest in the pipe, where the child blocks. A socket with output
  still queued is asked about again soon by `PlatformWatch::add(NetSocket)` itself, so
  the queue drains without a tick asked for.
- **A child's exit is not watched**, only its pipes (`PlatformWatch::add(Process)`): once
  they have ended, add `w.soon()` until `running()` says it is over. (On Windows the pipes
  themselves cannot be watched, and `add` asks soon for them already.)
- **How soon is soon**: `kPlatformSoon` is 4 ms, but a loop with no window waits on a
  condition, and Windows rounds that wait up to its scheduler tick, 15.6 ms unless
  something raised the timer resolution. A tool reading a child on Windows wakes about 60
  times a second; on POSIX the pipes are watched and it wakes when output comes.
- **Or an `Alertable`** ([platform](../platform/README.md), "Alerts"): an entity that
  reads one handle in a handler needs no waker. It hears only "something to read", so a
  tool that also waits on queued output, a time or a child's end uses a waker.

**In a game.** The same module linked into a game registers the same commands in the
game's CLI, so `kpack verify ...` can be typed into a running dev build. `tool_running(e)`
says whether a tool run is driving the engine, for a command that should behave
differently (a game's CLI has no `tool_finish` to wait for).

## Quick reference

Header `korvid/tool/tool.h`:

| name | kind | what |
|---|---|---|
| `ToolConfig` | record | `name` (the command prefix), `engine` (an `EngineConfig`) |
| `tool_main(argc, argv, modules, config)` | function | the whole program; returns the exit status |
| `tool_run(argc, argv, modules, config)` | function | the same without the process-wide shutdown, for tests |
| `tool_finish(e, code)` | function | the work is done; the first call stands |
| `tool_running(e)` | function | a tool run is driving this engine and has not finished |

Exit status: the `tool_finish` code; 1 if the engine did not start; 2 for a refused
request, a bad option or `--max-ticks`; 3 for a leak in a debug or dev build.

What the module registers with the engine: its state, nothing else.

## Behaviour worth knowing

- **Finishing is the tool's job.** A command that starts nothing and never calls
  `tool_finish` runs until `--max-ticks`, or forever. Every path through a tool's work,
  failures included, ends in a finish.
- **Ticks are not time.** A tool ticks when there is something to do, each tick the
  engine's tick length, so anything that counts ticks counts work, not seconds.

## Not in this module

Argument parsing beyond the engine's flags (a command gets its words and parses them),
progress bars, a daemon mode that keeps the engine up between requests.

## Seen in

Every tool in `tools/` except kormake: [kpack](../../tools/kpack/README.md) (I/O through
the queue, one verb per command), [goldberg](../../tools/goldberg/README.md),
[kbor](../../tools/kbor/README.md), [kimc](../../tools/kimc/README.md),
[picture](../../tools/picture/README.md) and [kvscript](../../tools/kvscript/README.md).

## See also

- [Cycles](../../docs/concepts/cycles.md): the Sim and Work cycles a tool runs.
- [frameloop](../frameloop/README.md): the loop for a program with a window, which a tool
  with a window uses instead.
- [platform](../platform/README.md): the I/O queue a tool reads and writes through.
