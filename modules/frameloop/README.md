# frameloop

The mechanical part of every game, so that no game has to write it: the command line, the
window and the renderer from a config, the frame (pump, ticks, the Work and Frame cycles,
render), pacing, resize, the fps line, and recording and replay. A game declares a
`constinit FrameLoopConfig`, and its `main` is one call to `frameloop_main`. When someone
wants out (the window closed, Escape, a timer), the game hears it as `ExitRequested` and
decides what to do. The loop ends when something calls `engine_request_quit`. The bus is
the seam: the game never touches the window, the renderer or the loop.

## How it works

`frameloop_main(argc, argv, config)` parses the command line over the config, sets up
the profiler if anything will read it, configures the CLI module, runs `engine_init` on
the generated module list, loops, then shuts everything down in order (engine, profiler,
names, memory). The module's own startup opens the window and creates the renderer at
the window's pixel size. Shutting them down is the platform's and the renderer's job.

One frame:

1. `platform_pump`: OS events onto the bus, raw input, the I/O queue's pump.
2. `engine_frame(dt)`: the Sim ticks the clock owes at 60 Hz, up to four.
3. The Work cycle (`engine_work`, [cycles](../../docs/concepts/cycles.md)). Its budget is
   the time the renderer blocked for last frame, less a margin. With work pending, it
   runs until the frame's deadline: the period, less what the frame has spent and what
   the render costs.
4. The Frame cycle (`engine_frame_cycle`), then the render. The window's size reaches the
   renderer at render rate. The sprite module's deltas are applied, and the world as of
   the last tick is drawn interpolated by the clock's alpha. The camera
   (`sprite.camera`) sets the view.
5. Once a second, the fps line: frames, tick, alpha, sprites, deltas per frame, size,
   focus, live memory, idle time.

**Pacing** is `FrameLoopConfig::pacing`. `Continuous`, a game's choice, draws every
vsync. `OnDemand`, a tool's choice, draws only when something happened: input, the
window, I/O, a load, ticks that moved sprites, `engine_want_frame`, or a waker that had
something (`engine_add_waker`). Between those, it runs no ticks, wakes at least four times
a second (the heartbeat), and still runs pending Work. `engine_want_ticks` is a frame
request there. `Decoupled`, a player's choice, decides ticks and frames apart: it ticks at
the clock's rate while a module asks (`engine_want_ticks`, again each tick) and while I/O
or Work is pending, and four times a second otherwise; it draws only on the same signals as
`OnDemand` less the heartbeat. **Between passes both wait** (`platform_wait`, KR26) for the
window, a wake, or the soonest of the heartbeat, `engine_want_frame_at`'s time, the next
tick when ticks are wanted, and `--run-seconds`' end; not at all while a frame is wanted,
Work is pending or the frames after input are owed. The wait is a 4 ms poll while I/O is
under way, a load holds the sim, or a waker is registered (the loop says so once). A still
window wakes about four times a second. The last log line says how many frames it drew,
how many ticks ran without one, and how many waits ended by the window, a wake and the
time. **A hidden window** (`WindowVisibility`) is drawn under no pacing:
`Continuous` ticks at the clock's rate and waits between, the others tick as `Decoupled`
would without drawing; the first pass it is seen again draws.

The system `frameloop.exit` (phase PreSim, `Always`) emits `ExitRequested` with a reason:

- `WindowClosed` when `CloseRequested` arrived and the config allows it;
- `Escape` when the key is held and the config allows it;
- `Timer` once, when `--run-seconds` has elapsed or `--run-ticks` is reached.

The game's handler or system reads it and quits when it is ready (after a fade, a save,
a goodbye), or ignores it.

## Using it

**A game's main:**

```cpp
#include "korvid/frameloop/frameloop.h"

namespace {
constinit const char* const k_flags[] = {"--demo"};   // flags the game accepts; frameloop_flag says which were given
constinit const kv::FrameLoopConfig k_config = {
    .title  = "my game",
    .width  = 1280,
    .height = 720,
    .flags  = k_flags,
    .engine = {.heaps = k_heaps, .module_heaps = k_module_heaps}, // optional budgets
};
}

int main(int argc, char** argv) { return kv::frameloop_main(argc, argv, k_config); }
```

**Leaving:** in a handler (it runs on paused ticks too), after whatever the game wants
to do first:

```cpp
void on_exit(TickContext& ctx, Span<const ExitRequested> exits) {
    if (exits.len) engine_request_quit(*ctx.engine);
}
```

**Flags:** `frameloop_flag(engine, "--demo")` is true if the flag was on the command line;
`frameloop_config(engine)` is the config in effect. **Options** are flags with a value
(`options = {"--dial"}` in the config). `frameloop_option(engine, "--dial", i)` is the
value after the `i`th occurrence, or null. An option may be given any number of times.

**Recording and replay.** `--record FILE` writes a run's input: the values of the actions
the config names in `recorded_actions`, for every sim tick since the last fresh sim
(`engine_sim_reset`; a file holds the last run). Name the gameplay actions; pause and quit
stay live. Values are stored as changes, with the state hash every 60 ticks. Paused and
held ticks are not sim time and record nothing, so menus, a pause, and a disk of any
speed leave the file unchanged. The header holds the executable's name and the game's
flags and options as given. For a run a menu chose, add the menu's choices with
`frameloop_record_note(e, "--arcade --difficulty hard")`.

`--replay FILE` takes those arguments as if they were given, and sets the named actions
from the file before each sim tick's sample. It uses `input_action_override_exact`,
because the sample's clamp would round a recorded stick a second time. It compares the
hashes as it goes, logs `replay: diverged at tick N` at the first that differs, and
`replay: ended at tick N, matched (H hashes)` at the end. `frameloop_replaying(e)` is true
meanwhile, so a game's own pilots can stand down.

The file (`.kvr`) starts with `KVR1` and a header of `exe`, `arg` and `action` lines. Then
come records, each a varint tick delta and a type: an action's index with its down state
and axis, a hash, or the end. It is read and written with stdio at startup and shutdown.

**A tool that drives frames itself** (cpukiller's window mode): `frameloop_set_config(cfg)`
before `engine_init`, then `frameloop_step(engine, dt)` per frame, which pumps, ticks and
renders once.

**The command line**, overriding the config:

| option | what |
|---|---|
| `--run-seconds N` | `ExitRequested{Timer}` after N seconds; for smoke tests and captures |
| `--run-ticks N` | `ExitRequested{Timer}` at sim tick N: a run the same length on any machine, however its frames are paced |
| `--size WxH` | the window's size over the config's |
| `--quiet` | log at Warn and above |
| `--validation`, `--no-validation` | the Vulkan validation layers (on in debug builds) |
| `--no-vsync` | mailbox or immediate presentation; no slack, so no idle work |
| `--pacing demand\|continuous\|decoupled` | `FrameLoopConfig::pacing` (above) |
| `--renderer auto\|gpu\|cpu` | which bound backend draws (`FrameLoopConfig::renderer`) |
| `--cpu-shaders all\|none` | whether the CPU renderer runs game shaders |
| `--cpu-scale auto\|N` | the CPU renderer's window pixels per drawn pixel, 1 to 8 |
| `--borderless` | no title bar; sidesteps the GNOME Wayland resize stall |
| `--threads N` | the job pool's size, main thread included; 1 is serial; the default is every core |
| `--seed N` | the engine's seed (`EngineConfig::seed`): every module's `ctx.rng()` starts from it and its name; a recording keeps it |
| `--rate X` | the developer's rate from the start, as the CLI's `rate X`: a recorded run or a demo four times faster |
| `--early-reads order\|snapshot\|ktable` | every ktable's early reads ordered, every ktable's from a snapshot, or as each ktable says (the default); the game must hash the same ([scheduling](../../docs/concepts/scheduling.md#early-reads-order-or-snapshot)) |
| `--hash`, `--hash-systems` | log the state hash after every tick; after every system |
| `--trace-events` | print every delivered event with its emitter (dev builds) |
| `--record FILE`, `--replay FILE` | write a run's input to a file; play a file back and check its hashes (above) |
| `--hitch-ms N` | log a frame longer than N ms with its pump, tick and render split, and its zone tree |
| `--profile FILE`, `--prof-report` | a Chrome trace; the top zones with every fps line. Either turns on GPU timing: a `gpu` line with every fps line (the GPU's frame, its sort and draw, overdraw: `renderer_gpu_stats`) and a `gpu` track in the zones and the trace |
| `--overlay` | the fps and memory lines (and with `--prof-report`, the top zones) on screen |
| `--screenshot FILE`, `--screenshot-frame N`, `--screenshot-tick N` | one presented frame as a binary PPM: frame 90 unless said otherwise, or the first at or past tick N, counting every tick except those a load held. The CLI's `screenshot FILE` takes the next frame drawn |
| `--check-sort` | read the GPU sort back every 30th frame and verify it |
| `--io sync\|ring` | the I/O queue's backend; `ring` (or `uring`) is the platform's async ring, the default where it exists |
| `--pack FILE` (repeatable), `--assets DIR` | mounts over `<exe dir>/assets/` and `<exe dir>/<exe>.kpak`, later ones winning |
| `--cli-port N`, `--cli-stdin`, `--cli-ascii` | the CLI's transports: loopback TCP, stdin even on a terminal; ASCII-only escapes non-ASCII as `\u{XXXX}` both ways |
| `--goldberg PORT`, `--no-goldberg` | for a program that links `asset_live`: the running goldberg watch to reload from, on this port; or none this run (they set `KV_GOLDBERG`) |
| `--idle-margin-ms N`, `--no-idle` | the slack kept back from the Work cycle (1.5 ms); one Work cycle a frame and no more |
| `--capacity KTABLE=N` (repeatable) | a ktable's capacity over the game's, by its full name (`arcfire.bullet=131072`) |
| `--sprite-rows N` | the first size of the renderer's sprite table and of the extract's, over the config's `sprite_rows` (0: the default) |
| `--touchpad` | the on-screen gamepad with the mouse as a finger, to try a layout on the desktop; it is on by itself where the screen is touched |
| `--spike-hunt` | a dev build without the work that does no checking: no memory fills or poison, no guard pages, no event emitters for the trace (`dev_spike_hunt`). Its timings are near a ship build's, and its asserts, access checks, counters and profiler stay |

## Quick reference

Header `korvid/frameloop/frameloop.h`:

| name | kind | what |
|---|---|---|
| `FrameLoopConfig` | record | `title`, `width`, `height`, `vsync`, `borderless`, `validation`, `renderer`, `cpu_shaders`, `cpu_scale`, `exit_on_close`, `exit_on_escape`, `run_seconds`, `run_ticks`, `clear_color`, `report_every_seconds`, `sprite_rows` (the first size of the renderer's sprite table and the extract's: a game that draws many asks, so the tables never grow mid-run), `touchpad_layout`, `flags`, `options`, `recorded_actions`, `engine` (an `EngineConfig`), `pacing` |
| `Pacing` | enum | `Continuous`, `OnDemand`, `Decoupled` |
| `ExitReason`, `ExitRequested{reason}` | enum, event | `WindowClosed`, `Escape`, `Timer` |
| `frameloop_main(argc, argv, config)` | function | the whole run; returns the process exit code (2 for a bad option, 1 for a failed init) |
| `frameloop_config(engine)` | function | the config in effect: the declaration plus the command line |
| `frameloop_flag(engine, flag)` | function | whether one of the declared flags was given |
| `frameloop_option(engine, option, i)` | function | the value after the `i`th occurrence of a declared option, or null |
| `frameloop_request_frame(engine)` | function | another frame, for code that does not include the engine's header (`engine_want_frame`) |
| `frameloop_replaying(engine)`, `frameloop_record_note(engine, args)` | functions | whether a replay is driving the recorded actions; a menu's arguments for the recording's header |
| `frameloop_set_config(config)`, `frameloop_step(engine, dt)` | functions | for a tool that drives the loop itself |

Header `korvid/frameloop/module.h`: `frameloop_module`, the record.

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `ExitRequested` | event type | no schema |
| `frameloop.exit` | system, phase PreSim | `Always`; reads `input.state`, `CloseRequested`; emits `ExitRequested`, one per reason per tick, paused or not |
| `frameloop.replay` | system, phase Input | main thread; before `input.sample`: the file's actions |
| `frameloop.record` | system, phase Input | main thread; after `input.sample`; reads `input.actions` |
| `frameloop.on_sim_reset` | handler, phase Sim | on `SimReset`: a recording starts again |
| `frameloop.overlay` | ktable | cosmetic; `Transform2`, `Text`: the `--overlay` lines; never hashed |
| `screenshot FILE` | CLI command | the next frame drawn, as a PPM |

Depends on: core, platform, platform_hid, math, render, asset, sprite, scene, text, cli,
touchpad. A game that depends on frameloop has all of them.

## Behaviour worth knowing

- **Ticks are 60 Hz, four per frame at most.** A frame that takes longer than four ticks
  drops the rest and the game slows; `Engine::clock` has the counters.
- **Render rate is the display's with vsync.** The renderer blocks in acquire, and that
  blocked time is next frame's Work budget. Without vsync the loop spins and there is no
  slack.
- **The window's size reaches the renderer at render rate**, not tick rate: if a frame is
  presented at a stale size during a live resize, the compositor withholds the next one.
- **Mouse resizing stalls on GNOME Wayland** for the whole drag, because libdecor repaints
  the frame on every configure. Keyboard resizes and `--borderless` are fine.
  `host_window.h` has the diagnosis.
- **The view is the camera's.** The loop reads `sprite.camera` at render rate and builds
  the world view from it: the window's pixel size over the zoom, centered and rotated.
  The default camera is one world unit per pixel with the origin at the window's center,
  +y up. Screen-space sprites ignore it.
- **Pause is the engine's.** `engine_pause` (or the CLI's `pause`) holds the sim. The loop
  keeps running paused ticks, so input and the CLI stay alive, and the Frame cycle (Krome,
  the extract) runs whatever the sim does. Handle `ExitRequested` in a handler (or an
  `Always` system), never in a Sim system: a quit requested during a pause would otherwise
  wait for the resume.
- **`--overlay` puts the report on screen**: the fps line and the memory line at the
  top-left, in the module's own cosmetic ktable, and with `--prof-report` the top zones
  under them, refreshed with every report. It draws in DejaVu Sans (the dejavu module) at 16 px.
- **`--run-seconds` is cooperative.** It emits `ExitRequested{Timer}` once; a game that
  ignores it runs on.
- **Everything mechanical is logged once**: the schedule at init, the device and the
  swapchain, each module up and down, and at exit the renderer's totals, including the
  validation counts, which are the acceptance gate.

## Not in this module

Fixed-size render targets and letterboxing, more than one window, a main menu, save and
load, and input mapping (that is [platform_hid](../platform_hid/README.md)'s action
map). The game owns those, on top of what the loop provides.

## Seen in

- [rocks](../../examples/rocks/README.md): a config with heaps and a touch layout;
  `ExitRequested` handled in `on_exit`; `--demo` as a declared flag.
- [arcfire](../../examples/arcfire/README.md): options (`--dial`), `recorded_actions`,
  recordings made from a menu with `frameloop_record_note`.
- [kromedemo](../../examples/kromedemo/README.md): on-demand pacing; `--work` fills the
  time between frames with a Work unit.
- [ticker](../../examples/ticker/src/main.cc): decoupled pacing; a module that asks for every
  tick and draws nothing new.
- [cpukiller](../../examples/cpukiller/README.md): `frameloop_set_config` and
  `frameloop_step` driving frames itself.

## See also

- [Frame loops and scheduling](../../docs/guides/frame-loop-and-scheduling.md): the guide
  to what one frame and one tick do.
- [Cycles](../../docs/concepts/cycles.md): Sim, Frame and Work, and who runs them.
- [tool](../tool/README.md): the loop for a tool with no window.
- [sprite](../sprite/README.md): the camera and the extract the loop draws from.
- [platform_hid](../platform_hid/README.md): the window, input, and the action map behind
  `recorded_actions`.
