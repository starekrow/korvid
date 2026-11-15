# Cycles

Nov 15, 2026. Three rhythms of work in one schedule: the simulation's fixed ticks, a
frame's worth of presentation once per frame shown, and throughput work that runs
whenever there is some. Written first, then built (see "As built").

## Why

The engine has one clock. Everything a system does happens in a tick: 60 a second, fixed,
deterministic, recorded and replayed. That is right for a game's simulation and wrong for
the two other kinds of work that live in the same process:

- **Presentation** wants a run per frame shown, paced by the display (or by demand), and
  never belongs in a replay: a UI's layout and paint, the pointer as the UI sees it, the
  extract to the renderer.
- **Throughput** wants to run as often as there is something to do: a tool's work
  (goldberg's loop, kpack's pump), the I/O queue's idle reads, background loads. Today a
  tool pretends its work is ticks ("ticks are not time", `modules/tool`), and a game's
  background work is an idle task, a mechanism of its own outside the schedule.

A tool with a window needs all three, and there was no honest way to give it them. Krome
(`docs/PRDs/Krome-Slice-1-PRD.md`, KS13) stopped there: rendering at vsync every tick caps a
tool's work at 60 ticks a second, and not rendering spins a core when idle.

## The model

A **cycle** is a run of phases with its own reason to run. There are three:

| cycle | phases | runs | for |
|---|---|---|---|
| **Sim** | Input, PreSim, Sim, PostSim | 0 to N times a frame, at the clock's fixed rate | the simulation: recorded, hashed, paused by holds |
| **Frame** | Frame | once before each frame is presented, and only then | presentation: a UI's input, layout and paint |
| **Work** | Work | over and over, while a unit says there is more, until the time it was given runs out | throughput: a tool's work, background loads, idle reads |

A system says where it runs as it says now, by phase: `.phase = Phase::Frame`,
`.phase = Phase::Work`. The two new phases are scheduled exactly as the four are: one table
of constraints, levels on the job pool, main-thread units first, early reads, snapshots,
commands applied and events merged at the boundary. Nothing about what a declaration
means changes; only when the phase runs.

### Who runs the cycles

The engine runs a cycle when asked; the loop decides when to ask.

- `engine_frame(e, dt)`: the Sim ticks the clock owes, as now.
- `engine_frame_cycle(e, dt)`: the Frame cycle once, `dt` the time since the last.
- `engine_work(e, budget)`: Work cycles back to back while any unit of the last one said
  `ctx.more()` and the budget lasts; the seconds it used.

**The frame loop** (`modules/frameloop`), every pass:

1. pump the platform;
2. the Sim ticks owed (`engine_frame`);
3. Work until the frame's deadline: the slack the renderer measured (what idle had), or
   for as long as there is work when nothing is due to draw;
4. when a frame is due: the Frame cycle, then the render;
5. on demand, with nothing due and no work: sleep.

**The tool loop** (`modules/tool`): pump, a tick (a tool's systems that are ticks stay
ticks), then Work for a few milliseconds; it yields the core only when nothing has work
and the disk is busy.

A tool with a window is then the frame loop with the tool's modules in it: the Work cycle
fills the time between frames, so the tool's throughput loses only what the frames cost,
and when there is no work the window paces on demand.

### Work says when it has more

A Work unit does a slice and returns. If it has more, it calls `ctx.more()`; the cycle then
runs again, every Work unit once more in schedule order, until a cycle where none said so or
the budget is spent. `ctx.time_left()` is the unit's share: what the cycle has left, split
evenly among the units still to run in it, so a unit that spends all of its share leaves
the others theirs. A unit with nothing to do returns at once and its time passes on.

### The Frame cycle draws

It runs before the render, once per frame drawn, never for a frame not drawn (on demand,
or a minimised window). It sees input as the frame has it: the pointer and the keys now
(`Input::raw`), not the tick's sample, since a frame between two ticks should still see a
hover. Its `dt` is the frame's: what a transition animates by.

### Events and resources across cycles

- **Events are delivered at the consumer's next run.** Streams are read by cursor; a unit
  in the Frame cycle that reads an event the Sim emits gets everything since it last ran,
  however many ticks that was. The other way, an event the Frame emits reaches a Sim
  reader at its next tick. Streams keep what a consumer has not reached, as now; trimming
  after a cycle never moves the tick's own window (`read()`), so a Sim reader misses
  nothing emitted between ticks.
- **Resources are shared one cycle at a time.** Cycles never overlap: each runs to its
  end, boundary included, before another starts. Within a cycle the scheduler checks as
  it checks a phase. Across cycles there is nothing to race.
- **Commands** (spawns, destroys, appends) apply at the cycle's boundary.
- **Order across cycles** is the loop's, not the solver's: `after` and `before` naming a
  system in another cycle say nothing about order and are accepted; a dependency between
  cycles is an event or a resource.

### Determinism

The Sim cycle is the only one in a replay. The other two run as often as the machine
allows, so:

- **They do not write the simulation.** A Frame or Work unit may read any kind and write
  cosmetic kinds, cosmetic components and its own state. A declared write that covers a
  sim kind (the kind, its liveness, a column, an attachment, its bags) is refused at
  startup, naming the unit and the resource; a spawn, a destroy or a `destroy_all` into a
  sim kind, and any timer (`schedule`), is refused when issued. Events are the way in.
- **What they emit to the Sim is input from outside**, as an event from code outside the
  schedule is today: a game that lets a UI drive its simulation records those events, as
  it records input. That is the replay question Krome deferred, now in one place.
- **Pausing** holds the Sim only. Frame and Work run whatever the clock's holds are.

### Idle became Work

An idle task was Work by another name: a slice, then More, Nothing or Done. The one there
was, the I/O queue's idle reads, is the platform's Work unit `platform.io_idle`, and
`engine_idle_*` is gone.

### Pacing

The Frame cycle carries the frame's pacing: continuous, a frame every vsync; on demand, a
frame when input, the window, a load or `engine_want_frame` asks. Work never draws, so it
never asks for a frame; it only keeps the loop from sleeping.

## What it costs

A Frame and a Work cycle with nothing in them cost a phase's bookkeeping: a few
microseconds. The Work cycle's loop costs a cycle's bookkeeping per repeat, so a Work unit
should do a useful slice (hundreds of microseconds), not one item.

## Prior art

Bevy's `FixedMain` (0..N times a frame) against `Update` (once a frame); Unity's
`FixedUpdate` against `Update`; Godot's `_physics_process` against `_process`. The Work
cycle is the piece they leave to threads or coroutines.

## Not now

- **Work on its own thread.** Cycles run on the main loop; a Work unit fans out to the
  job pool as any unit does. A cycle that runs beside the others needs the resources
  split by cycle, and nothing asks for it yet.
- **More than one Frame cycle** (one per window).
- **A tool's command line through the frame loop.** A windowed goldberg needs its CLI
  request run the way `tool_main` runs it; that is goldberg's window's to settle.

## Open questions

- [x] The extract: moved to the Frame cycle (As built).
- [x] A per-unit budget in Work: even shares (As built).
- [x] Refusing sim writes from Frame and Work: both (As built).

## As built

Nov 15, 2026, on branch tool-stuff.

- **Core.** `Phase::Frame` and `Phase::Work` after the four Sim phases (`KV_SIM_PHASES`);
  `PhaseList::run_cycle` runs one as a tick runs a phase, levels and boundary, and trims the
  streams (`EventBus::trim`) without moving the tick's window. `engine_frame_cycle`,
  `engine_work`, `engine_work_pending`, `ctx.more()`, `ctx.time_left()`. `core_tests`
  checks the cycles (events both ways, repeats, every unit once a cycle, the budget,
  pause, commands, trimming).
- **Idle tasks are gone** (later the same day). The platform's idle reads are a Work unit,
  `platform.io_idle`; `sockit2m` hands its slack to `engine_work`; `engine_idle_*`,
  `IdleStats` and `idle_test` are removed. A tool now serves idle reads too, in the 5 ms
  it gives Work.
- **The frame loop.** Work runs where idle ran. With work pending its budget is a deadline,
  not idle's slack: the frame period (smoothed) less what this frame has spent, what a
  render costs the CPU besides its wait, and the margin. Idle's slack alone shrank as work
  filled it: the wait it measures is the time work took, so the budget fell every frame
  (42% of a run went to the work; with the deadline, 86%). The Frame cycle runs before
  each render; on demand, pending work runs without drawing.
- **The tool loop** gives Work 5 ms after each tick, and does not yield while work is
  pending.
- **Krome** is in the Frame cycle. It reads presses, releases, the wheel and Escape as
  events (none lost or doubled however frames and ticks fall) and where the pointer is
  from the raw state. `input_pointer_override` emits the events a mouse would.
- **A tool with a window** is the frame loop with Work units: `kromedemo --work` counts
  primes in the Work cycle behind its window, 86 to 87% of the wall time computing on
  continuous or on-demand pacing, 40 frames drawn in the 0.8 s it takes.
- **The extract is in the Frame cycle** (later still). It ran every tick and appended a
  tick's deltas each time; now it runs once before each frame and compares against what it
  last told the renderer, which was always its test, so nothing a tick changed is missed.
  Two things keep interpolation as it was: a frame with no tick sends no `Tick` marker
  (the renderer keeps interpolating the last tick), and a frame with several sends each
  moved sprite first to its previous copy, with snap, so only the last tick's motion is
  interpolated. Arcfire's ring bench: the ticks 0.5 ms a frame cheaper, the Frame cycle
  0.48 ms, the extract itself the same; the replay matches.
- **Frame and Work leave the sim alone** (last). At startup, on each unit's expanded
  access: a write reaching a resource of a non-cosmetic kind that is not itself a cosmetic
  component fails init with the unit, its cycle and the resource named. At run time, in
  the context: spawns, destroys and `destroy_all` into a sim kind, and every `schedule`,
  are refused and logged (a spawn before anything reaches the command buffer, so nothing
  half-written is left). Always on, not only in dev builds: they are cheap, and a replay
  that drifts is not. Nothing in the tree tripped them; `core_tests` covers both halves. Arcfire's recorded replay matches (236 hashes): the Sim cycle
  is unchanged.
- **Presentation moved, and the follow-ups** (last of the day). `text.layout`, `ui.draw`
  and `touchpad.draw` are in the Frame cycle; `ui.update` stays in the tick, since reveals
  and UI events feed the game. Text places new glyphs itself (`placement_compose`), so the
  frame that shows them shows them in place. Moving layout took away an order nothing had
  declared: rocks' hit and spawn handlers and `anim.play` and `scene.compose` had been
  ordered only through `text.layout`, so the handlers now say `before` (the hashes are
  unchanged), and `anim.play` no longer names `text.layout`. Cross-cycle `after`/`before`
  are accepted, as this note always said; the code refused one before. Work units get
  even shares of the cycle (`ctx.time_left`). Layers keep their offsets as each tick
  begins (`sprite.layers_tick`) and the extract catches them up as it does sprites.
  Dev builds name a stream held past 65536 events by a unit that has not run, with its
  cycle; a program that never runs the Frame cycle at all is otherwise silent, since a
  headless test of a game legitimately never draws.

