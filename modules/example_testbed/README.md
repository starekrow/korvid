# example_testbed

The smallest game that uses everything once: a kind, a view system, a handler, timers,
a command spawn with a ticket, a trigger, assets from files, and a state machine that
never holds a handle. Four balls bounce in a window over a tiled checkerboard; one of
them blinks out of existence and back on a schedule; a log line marks a ball's third
bounce. It is the smoke test and the place to try a feature before a real game needs
it. `--swarm` turns it into a renderer stress.

## Building and running

From the repository root, once, to build the build tool:

```bash
tools/kormake/bootstrap.sh
```

Then build the configuration you want and run the executable from its bin directory,
where the shaders and this module's `assets/` have been copied next to it:

```bash
tools/kormake/bin/linux/kormake build linux-debug
build/linux-debug/bin/example_testbed --run-seconds 3
```

`kormake build linux-debug` is everything checked with the validation layers on;
`linux-dev` is the build to measure with; `linux-ship` has neither. `kormake build`
builds every module and example; `kormake test linux-debug` runs the test suites.

## What it shows

**A kind with the game's own components.** `example_testbed.ball` is `Transform2`,
`Sprite`, `Motion` (velocity and a "re-aim on bounce" flag) and `Bounces` (a count, with
`CompPrev` so a trigger can watch it).

**A view system.** `example_testbed.move` (Sim) iterates every kind with `Transform2`,
`Motion` and `Sprite` through `ctx.view`, moves each ball, keeps it inside the window
(read from the `platform_hid.window` resource), and emits `Bounced{ball}` at an edge. It
declares exactly what it touches; the scheduler puts `example_testbed.sim` after it because `sim`
reads the kind `move` writes.

**A state machine as a system.** `example_testbed.sim` (Sim) reads keys, asset events and
`ExitRequested` from the bus, then runs INIT (spawn the background and the balls by
command), RUN, EXIT (destroy everything by command, quit). It holds no handles and no
writes: spawning and destroying are commands, and the one handle it needs comes back
through an event.

**Handlers at the boundaries.** `on_bounced` increments `Bounces` and re-aims the random
ball, indexing the columns directly because a delivered handle is live. `on_spawned`
matches the blinker's spawn ticket to learn its handle and schedules `BlinkOff` 45 ticks
out. `on_blink_off` destroys it and schedules `BlinkOn` 30 ticks out; `on_blink_on`
spawns a new one by command, and the dance repeats. `on_edge` logs the trigger.

**A trigger.** `example_testbed.third_bounce` watches `example_testbed.ball.Bounces.count`
crossing above 2 and emits `StateEdge` without anyone polling.

**Assets from files.** Startup loads five material files (`white`, `blue_add`, `green`,
`magenta`, `background`) and keeps the renderer rows they fill; the balls use them
before the files arrive and change look when they do. `AssetReady` and `AssetFailed` are
logged by the state machine.

**The swarm.** `--swarm` spawns 40,000 small balls on random layers across the whole
range (beyond the renderer's clamp on purpose) in every look, plus 2,000 sprites parked
far outside the view that the GPU cull must drop. With `--check-sort` the renderer reads
its sort back and verifies it; the log reports rows, visible rows and buckets in use.

## Where things are

| file | what |
|---|---|
| `src/main.cc` | the `FrameLoopConfig` (960x540, the `--swarm` flag) and one call to `frameloop_main` |
| `src/game.cc` | everything above: components, the kind, events, the trigger, the systems, the handlers, startup |
| `assets/ball.png`, `assets/*.material.json` | the ball and its five looks |

What the module registers: components `Motion`, `Bounces`; kind `example_testbed.ball`
(65,536 slots, for the swarm); events `Bounced`, `BlinkOn`, `BlinkOff`; systems
`example_testbed.move`, `example_testbed.sim` (Sim); handlers `on_bounced`, `on_spawned`,
`on_blink_off`, `on_blink_on`, `on_edge`; trigger `example_testbed.third_bounce`.

## Behaviour worth knowing

- The blinker's ticket dance (spawn by command, learn the handle from `Spawned`, then
  schedule against it) is the honest way to schedule something against an entity that
  does not exist yet. A system holding the kind's write could spawn directly and skip it.
- The background is a `sprite.entity`, the sprite module's own kind: a sprite that is
  nothing else needs no kind of the game's.
- Keys are logged as they arrive, as a check that the input path is alive; the balls do
  not react to them.
