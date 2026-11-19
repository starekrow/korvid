# Cycles

The engine has three rhythms of work, and each one is a **cycle**: a run of phases with
its own reason to run. Every system belongs to exactly one, picked by its `.phase`. All
three are scheduled by the same rules ([scheduling](scheduling.md)); they differ only in
when they run and what they may change. The design record is
[docs/design/cycles.md](../design/cycles.md).

| cycle | phases | runs | for | in a replay |
|---|---|---|---|---|
| **Sim** | `Input`, `PreSim`, `Sim`, `PostSim` | 0 to 4 times a frame, at a fixed 60 Hz | the simulation: gameplay, physics, rules | yes: recorded, hashed, held by a pause |
| **Frame** | `Frame` | once before each frame drawn, and only then | presentation: UI layout and paint, the sprite extract | no |
| **Work** | `Work` | repeatedly, while a unit calls `ctx.more()` and time remains | throughput: a tool's work, background loads, idle reads | no |

## Who runs them

The engine runs a cycle when asked. The loop decides when to ask:

| call | runs |
|---|---|
| `engine_frame(e, dt)` | the Sim ticks the clock owes for `dt`, returning how many |
| `engine_frame_cycle(e, dt)` | the Frame cycle once; `dt` is the time since the last |
| `engine_work(e, budget)` | Work cycles back to back while a unit said `ctx.more()` and the budget lasts; returns the seconds used |
| `engine_work_pending(e)` | whether the last Work cycle had more to do, so a loop knows not to sleep |

A game never makes these calls itself, because `frameloop` does
([the guide](../guides/frame-loop-and-scheduling.md#one-frame) walks through its order).
A tool's loop is `tool`'s. A test or a custom loop calls them directly.

## The Sim cycle: ticks

A fixed-step clock (`TickClock`, `korvid/core/tick.h`) collects frame time and pays it
out in ticks of `tick_dt` (1/60 s). At most `max_ticks_per_frame` (4) run per frame, and
time past that cap is dropped, so a slow machine slows the game rather than spiralling.
`alpha()` is how far the clock is between the last tick and the next. The renderer
interpolates by it.

Each tick runs its four phases with boundaries between them. At a boundary, events are
merged into their streams and commands (spawns, destroys, appends) are applied. Timers
fire at the tick's start, `CompPrev` columns are copied at the top of PreSim, and
triggers are evaluated at the end. [Modules](modules.md#how-code-flows-through-a-tick)
has the full sequence.

**Pause, holds and time.** The sim is held while any hold is set (`engine_hold`;
`engine_pause` sets `HoldUser`). A held clock still produces ticks, but they are *paused
ticks*: the sim tick does not advance, timers and triggers wait, and only systems with
`.runs = RunWhen::Always` run. Input, loads, the CLI and the frame loop's exit are
`Always`, so a paused game still answers. Handlers run whenever they have events.
`engine_step(e, n)` lets `n` ticks through a pause. `engine_rate` (the developer's) and
`engine_time_scale` (the game's) change how many ticks run per second, never how long one
is, so a script counted in ticks plays out the same.

## The Frame cycle: once per frame drawn

The Frame cycle runs just before each render and never for a frame that is not drawn
(on-demand pacing with nothing to show, a minimized window). It sees input as the frame
has it: the raw pointer and keys, not the tick's sample, so a hover between two ticks
still shows. Its `dt` is the frame's, for transitions. `text.layout`, `ui.draw`,
`touchpad.draw` and `sprite.extract` live here.

## The Work cycle: while there is work

A Work unit does a slice of work and returns. If it has more, it calls `ctx.more()`, and
the cycle runs again (every Work unit once more, in schedule order) until no unit asks
again or the budget is spent. `ctx.time_left()` is the unit's fair share: the time left
in the cycle divided among the units still to run. A unit with nothing to do returns at
once and passes its time on. Aim for slices of hundreds of microseconds: each repeat costs
a cycle's bookkeeping. `platform.io_idle`, the I/O queue's idle reads, is a Work unit.

## The rule: only the Sim writes the sim

Frame and Work run as often as the machine allows, so nothing they do can appear in a
replay. They may read any kind, and write cosmetic kinds, cosmetic components and their
own module's state. Beyond that:

- a declared write that covers a sim kind (the kind, its liveness, a column, an
  attachment, its bags) fails `engine_init`, naming the unit, its cycle and the resource;
- a spawn, destroy or `destroy_all` into a sim kind, or any timer, is refused when issued
  and logged. These checks are always on, ship builds included.

To affect the sim, a Frame or Work unit emits an event. To the simulation that event is
input from outside, and a game that lets a UI drive its sim records those events as it
records input.

## Events across cycles

Streams are read by cursor, so a consumer gets everything emitted since it last ran,
whichever cycle it is in. A Frame unit reading a Sim event sees every tick's worth since
the last frame. A Sim unit reading a Frame event sees it at its next tick. `after` and
`before` naming a unit in another cycle are accepted and mean nothing: cycles never
overlap, and the loop decides their order.

## See also

- [Scheduling](scheduling.md): how units in one phase are ordered.
- [frameloop](../../modules/frameloop/README.md): the loop a game uses, and its pacing.
- [tool](../../modules/tool/README.md): the loop a command-line tool uses.
- `korvid/core/engine.h`, `korvid/core/tick.h`, `korvid/core/phase.h`.
