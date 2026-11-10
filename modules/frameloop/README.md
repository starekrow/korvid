# frameloop

The mechanical part of every game, so no game writes it: options, the window and the
renderer from a config, the frame (pump, ticks, idle work, extract, render), resize,
the fps line, and the three reasons a run ends turned into one event. A game declares a
`constinit FrameLoopConfig` and its `main` is one call to `frameloop_main`. The game
learns that someone wants out through `ExitRequested` and decides what to do; the loop
ends when something calls `engine_request_quit`. The bus is the seam: the game never
touches the window, the renderer or the loop.

## How it works

`frameloop_main(argc, argv, config)` parses the command line over the config, sets up
the profiler if anything will read it, configures the CLI module, runs `engine_init` on
the generated module list, loops, then shuts everything down in order (engine, profiler,
names, memory). The module's own startup opens the window (Vulkan surface) and creates
the renderer at the window's pixel size; shutdown is the platform's and the renderer's.

One frame:

1. `platform_pump`: OS events onto the bus, raw input, the I/O queue's pump.
2. `engine_frame(dt)`: the ticks the clock owes at 60 Hz, up to four.
3. Idle work: the time the renderer blocked for last frame, less a margin, goes to
   `engine_idle_run` (idle-priority loads, whatever modules registered).
4. The render: the window's size reaches the renderer at render rate, the sprite
   module's deltas are applied, and the world as of the last tick is drawn interpolated
   by the clock's alpha; one world unit is one pixel with the origin at the center.
5. Once a second, the fps line: frames, tick, alpha, sprites, deltas per frame, size,
   focus, live memory, idle time.

The system `frameloop.exit` (phase PreSim, reads `input.state`) emits `ExitRequested`
with a reason: `WindowClosed` when `CloseRequested` arrived and the config allows it,
`Escape` when the key is held and the config allows it, `Timer` once when `--run-seconds`
has elapsed. The game's handler or system reads it and quits when it is ready (after a
fade, a save, a goodbye), or ignores it.

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

**Leaving:** in a system, `for (const ExitRequested& e : ctx.events->read<ExitRequested>()) engine_request_quit(*ctx.engine);`,
after whatever the game wants to do first.

**Flags:** `frameloop_flag(engine, "--demo")` is true if the flag was on the command line;
`frameloop_config(engine)` is the config in effect. **Options** are flags with a value
(`options = {"--dial"}` in the config): `frameloop_option(engine, "--dial", i)` is the
value after the `i`th occurrence, or null; an option may be given as often as the game
likes.

**A tool that drives frames itself** (cpukiller's window mode): `frameloop_set_config(cfg)`
before `engine_init`, then `frameloop_step(engine, dt)` per frame, which pumps, ticks and
renders once.

**The command line**, overriding the config:

| option | what |
|---|---|
| `--run-seconds N` | `ExitRequested{Timer}` after N seconds; smoke tests and captures |
| `--quiet` | log at Warn and above |
| `--validation`, `--no-validation` | the Vulkan validation layers (on in debug builds) |
| `--no-vsync` | mailbox or immediate presentation; no slack, so no idle work |
| `--borderless` | no title bar; sidesteps the GNOME Wayland resize stall |
| `--hitch-ms N` | log a frame longer than N ms with its pump, tick and render split, and its zone tree |
| `--hash`, `--hash-systems` | the state hash after every tick; after every system |
| `--trace-events` | print every delivered event with its emitter (dev builds) |
| `--threads N` | the job pool's size, main included; 1 is serial |
| `--profile FILE`, `--prof-report` | a Chrome trace; the top zones with every fps line |
| `--screenshot FILE`, `--screenshot-frame N` | one presented frame as a binary PPM (frame 90 unless said) |
| `--check-sort` | read the GPU sort back every 30th frame and verify it |
| `--io sync`, `--io uring` | the I/O queue's backend (the default picks io_uring where the kernel has it) |
| `--pack FILE` (repeatable), `--assets DIR` | mounts over `<exe dir>/assets/` and `<exe dir>/<exe>.kpak`, later ones winning |
| `--cli-port N`, `--cli-stdin`, `--cli-ascii` | the CLI's transports; ASCII-only escapes non-ASCII as `\u{XXXX}` both ways |
| `--idle-margin-ms N`, `--no-idle` | slack kept back from idle work (1.5); none at all |
| `--capacity KIND=N` (repeatable) | a kind's capacity over the game's, by its full name (`example_arcfire.bullet=131072`) |
| `--sprite-rows N` | the renderer's first sprite table size over the config's `sprite_rows` |

## Quick reference

Header `korvid/frameloop/frameloop.h`:

| name | kind | what |
|---|---|---|
| `FrameLoopConfig` | record | `title`, `width`, `height`, `vsync`, `borderless`, `validation`, `exit_on_close`, `exit_on_escape`, `run_seconds`, `clear_color`, `report_every_seconds`, `sprite_rows` (the renderer's first sprite table size: a game that draws many asks, so the tables never grow mid-run), `flags`, `options`, `engine` (an `EngineConfig`) |
| `ExitReason`, `ExitRequested{reason}` | enum, event | `WindowClosed`, `Escape`, `Timer` |
| `frameloop_main(argc, argv, config)` | function | the whole run; returns the process exit code (2 for a bad option, 1 for a failed init) |
| `frameloop_config(engine)` | function | the config in effect: the declaration plus the command line |
| `frameloop_flag(engine, flag)` | function | whether one of the declared flags was given |
| `frameloop_option(engine, option, i)` | function | the value after the `i`th occurrence of a declared option, or null |
| `frameloop_set_config(config)`, `frameloop_step(engine, dt)` | functions | for a tool that drives the loop itself |

Header `korvid/frameloop/module.h`: `frameloop_module`, the record.

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `ExitRequested` | event type | no schema |
| `frameloop.exit` | system, phase PreSim, `Always` | reads `input.state`; one event per reason per tick, paused or not |
| `frameloop.overlay` | kind, cosmetic | `Transform2`, `Text`: the `--overlay` lines; never hashed |

Depends on: core, platform, math, render, asset, sprite, scene, text, cli. A game that depends on frameloop
has all of them.

## Behaviour worth knowing

- **Ticks are 60 Hz, four per frame at most.** A frame that takes longer than four ticks
  drops the rest and the game slows; `Engine::clock` has the counters.
- **Render rate is the display's with vsync.** The renderer blocks in acquire, and that
  blocked time is next frame's idle budget. Without vsync the loop spins and there is no
  idle time.
- **The window's size reaches the renderer at render rate**, not tick rate: a frame
  presented at a stale size during a live resize makes the compositor withhold the next.
- **Mouse resizing stalls on GNOME Wayland** for the whole drag (libdecor repaints the
  frame per configure). Keyboard resizes and `--borderless` are fine. `host_window.h` has
  the diagnosis.
- **The view is the camera's.** The loop reads `sprite.camera` at render rate and builds
  the world view from it: the window's pixel size over the zoom, centered and rotated.
  The default camera is one world unit per pixel with the origin at the window's center,
  +y up. Screen-space sprites ignore it.
- **Pause is the engine's.** `engine_pause` (or the CLI's `pause`) holds the sim; the
  loop keeps running paused ticks, so input, the extract and the CLI stay alive. Handle
  `ExitRequested` in a handler (or an `Always` system), never in a Sim system: a quit
  asked for during a pause would otherwise wait for the resume.
- **`--overlay` puts the report on screen**: the fps line and the memory line at the
  top-left in the module's own cosmetic kind, and with `--prof-report` the top zones
  under them, refreshed with every report. It loads `text/DejaVuSans.ttf` at 16 px.
- **`--run-seconds` is cooperative.** It emits `ExitRequested{Timer}` once; a game that
  ignores it runs on.
- **Everything mechanical is logged once**: the schedule at init, the device and the
  swapchain, each module up and down, and the renderer's totals at exit including the
  validation counts, which are the acceptance gate.

## Not in this module

Fixed-size render targets and letterboxing, more than one window, a main menu, save and
load, and any input mapping. The game owns those on top of what the loop provides.
