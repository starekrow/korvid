# Frame loops and scheduling

What happens between `main` and a frame on screen, and how the engine decides when your
code runs. By the end you will know where to put a system, how to read the order the
engine chose, what its startup errors mean, and how to check that a change left the
game the same.

The guide uses [rocks](../../examples/rocks/README.md), an asteroids-like game in one
file (`examples/rocks/src/game.cc`). Build it and keep it running beside you:

```bash
cd examples && kormake rocks
```

```bash
build/linux-debug/bin/rocks --demo
```

## The shape of a run

A game's `main` is one call. This is `examples/rocks/src/main.cc`, minus its heap budgets
and touch layout:

```cpp
constinit const kv::FrameLoopConfig k_config = {
    .title  = "korvid rocks",
    .width  = 1280,
    .height = 720,
    .exit_on_escape = false, // Escape pauses; Q quits
    .flags  = k_flags,
};

int main(int argc, char** argv) { return kv::frameloop_main(argc, argv, k_config); }
```

`frameloop_main` parses the command line over the config, then calls `engine_init` with
the module list kormake generated from `module.lua`'s deps. `engine_init` reads every
module's `ModuleDesc`, gives every component, kind, event and system its id, builds the
schedule, and starts the modules in dependency order. Then the loop runs until something
calls `engine_request_quit`. After that, `engine_shutdown` stops everything in reverse
order. The game itself is a module, `rocks`, that declares tables. It never runs a loop
or opens a window.

## One frame

Every pass of the frame loop does the same five things:

```
frame
  platform_pump          OS events onto the bus, raw input, the I/O queue's completions
  engine_frame(dt)       the Sim ticks the clock owes: 0, 1, ... at most 4
  engine_work(budget)    the Work cycle, in the frame's spare time
  engine_frame_cycle(dt) the Frame cycle, once, for the frame about to be drawn
  render                 the world as of the last tick, interpolated by the clock's alpha
```

The simulation runs at a **fixed 60 Hz**, whatever the display does. The clock collects
each frame's time and pays it out in ticks of exactly 1/60 s. On a 120 Hz display, about
every other frame runs no tick. On a slow frame, two or three ticks run back to back. If
more than four are owed, the rest of the time is dropped and the game slows down rather
than falling further behind. Because ticks rarely land on frame boundaries, the renderer
draws each sprite between where it was at the last two ticks, by the clock's `alpha`.
Motion stays smooth, and the simulation never sees a variable `dt`: `ctx.dt` is always
1/60.

## Three cycles

Your code runs in one of three **cycles**, chosen by its system's `.phase`:

| cycle | phases | runs | put here |
|---|---|---|---|
| Sim | `Input`, `PreSim`, `Sim`, `PostSim` | each tick | the game: anything that changes what happens |
| Frame | `Frame` | once per frame drawn | presentation: UI layout and drawing |
| Work | `Work` | in spare time, while there is work | background work: a tool's jobs, loads |

Almost all game code goes in the Sim cycle. The Sim is the part the engine records,
hashes and replays, and that is why Frame and Work units [may not change
it](../concepts/cycles.md#the-rule-only-the-sim-writes-the-sim). [Cycles](../concepts/cycles.md)
has the full rules.

## One tick

A tick runs four phases in a fixed order:

| phase | for | in rocks |
|---|---|---|
| `Input` | sampling devices; reacting to last tick's news | `input.sample` (platform_hid), handlers for timers that fired |
| `PreSim` | housekeeping before the game moves | the asset pumps, `rocks.pause` |
| `Sim` | gameplay | `rocks.control`, `rocks.move`, `rocks.wrap`, `collision.resolve` |
| `PostSim` | reactions to what this tick did | `rocks.on_overlap`, `rocks.on_rock_hit`, `rocks.hud` |

Between phases there is a **boundary**. That is when the commands issued during the
phase take effect (`ctx.spawn_later`, `ctx.destroy`, appends such as `audio_play`), and
the events emitted during it are merged into their streams. A bullet destroyed in Sim is
still there for the rest of Sim, and gone in PostSim. Timers fire at the start of the
tick. `CompPrev` columns (last tick's copy) are taken at the top of PreSim.
[Modules](../concepts/modules.md#how-code-flows-through-a-tick) has the full sequence.

## Placing a system

A system is a function plus a row in the module's table saying where it runs and what it
touches. These are rocks' movement systems:

```cpp
constinit const SystemDesc k_systems[] = {
    {.name = ".control", .phase = Phase::Sim, .fn = control, .reads = "input.state",
     .writes = ".ship.Transform2, .ship.Velocity, .ship.Ship, .bullet, .flame",
     .appends = "audio.commands", .before = ".move"},
    {.name = ".move", .phase = Phase::Sim, .fn = move, .reads = "*.Velocity",
     .writes = "*.Transform2", .state = StateUse::None},
    {.name = ".wrap", .phase = Phase::Sim, .fn = wrap, .reads = "*.Velocity",
     .writes = "*.Transform2", .after = ".move", .state = StateUse::None},
    // ...
};
```

`reads` and `writes` name **resources**. `.ship.Velocity` is one column of one kind, and
`*.Velocity` is that column in every kind that has one. `input.state` is a resource
another module published. A name that starts with `.` is this module's own, so `.move` is
`rocks.move`. `.state = StateUse::None` says the system doesn't touch the module's own
state, which leaves it free to run beside the module's other systems.

You never write the order out. The engine solves it from the sets:

- a system that reads something runs **after every system that writes it** in the phase;
- two systems that write the same thing need an order, so one of them says `after` or
  `before`. `move` and `wrap` both write `*.Transform2`, so `wrap` says `after = ".move"`;
- systems with no path between them run **at the same time**, on the job pool.

The sets also limit what the code may touch. This is `move`:

```cpp
void move(TickContext& ctx) {
    f32 dt = (f32)ctx.dt;
    for (KindRows k : ctx.view<Transform2, const Velocity>()) {   // every kind with both
        Span<Transform2>     t = k.col<Transform2>();
        Span<const Velocity> v = k.col<const Velocity>();
        parallel_for(ctx, k.rows(), 512, [&](ChunkContext&, u32 begin, u32 end) {
            for (u32 i = begin; i < end; ++i) {
                if (!k.alive(i)) continue;
                t[i].position += v[i].v * dt;
                t[i].rotation += v[i].spin * dt;
            }
        });
    }
}
```

`ctx.view<Transform2, const Velocity>()` asks for a write on one column and a read on the
other, in every kind that has both. In a dev build, any access that is not in the sets
stops the program at once. Take `*.Velocity` out of `move`'s reads and rocks prints:

```
[ERROR] engine: system 'rocks.move' reads column 'rocks.ship.Velocity' without declaring it
```

That check is what keeps the order honest. The engine orders systems by what they
declare, so an undeclared read could see a value from before or after its writer,
depending on the schedule.

`parallel_for` splits a loop into chunks across the job pool. The engine merges each
chunk's events and commands in chunk order afterwards, so the result is the same however
many threads ran it.

## Reading the schedule

At startup the engine logs every edge it found and why, then each phase's order, with
`|` between **levels**. Units in one level run at once, and the next level waits for all
of them. This is rocks' Sim phase:

```
engine: phase sim:
engine:   collision.resolve after rocks.move: rocks.ship.Transform2 and 2 more
engine:   rocks.move after rocks.control: rocks.ship.Velocity and 2 more
engine:   rocks.wrap after rocks.move: declared
engine:   rocks.game after rocks.control: rocks.state, in declaration order
...
engine: order sim     collision.on_sim_reset, frameloop.on_sim_reset, rocks.control | rocks.move, rocks.game | rocks.wrap, rocks.on_exit | collision.resolve, rocks.on_asset_failed | collision.compose, collision.broadphase
```

Each edge gives its reason:

- **a resource:** `move` reads `rocks.ship.Velocity`, which `control` writes;
- **`declared`:** an `after` or `before`;
- **module state:** `rocks.game` and `rocks.control` both use rocks' state, so they keep
  declaration order.

The collision module ran its resolve after rocks' movers even though neither module
names the other, because it reads `Transform2` and they write it. That's how modules
compose: each declares what it touches, and the order across modules falls out.

When something runs in an order you didn't expect, this log is the first place to look.

## When init refuses

The engine checks the whole schedule at startup and fails `engine_init` with names
rather than running something that could race. Remove `wrap`'s `after = ".move"` and
rocks won't start:

```
[ERROR] engine: 'rocks.move' and 'rocks.wrap' both write 'sprite.entity.Transform2' (sim) with no order between them: add `after` or `before`
```

The three refusals you will meet:

| refusal | means | fix |
|---|---|---|
| two writers with no order | two units write one resource and nothing orders them | `after` or `before` on one of them |
| a cycle | A must run after B and B after A, through what they read and write | move one to another phase, read last tick's copy (`CompPrev`), or read early (below) |
| a torn read | one unit reads one thing early and another as it stands from the same writer | say which value it means, or make the kind a snapshot |

## Reading early

Sometimes a system wants a value **as the phase found it**, before this phase's writers
change it. An AI deciding where to go reads where everything stands, and then a later
system moves things. A `^` in front of the name says so: `.reads = "^.npc.Transform2"`. The
reader then runs *before* the writers, or, for a kind declared with
`EarlyReads::Snapshot`, from a copy taken at the start of the phase. Either way it sees
the same values. [Scheduling](../concepts/scheduling.md#early-reads-order-or-snapshot)
covers when to pick which.

## Events, handlers and timers

Systems talk through **events**. A unit declares what it emits (`emits = "RockHit"`).
A unit that reads the event type runs after every emitter of it in the phase and gets
everything emitted since it last ran. A **handler** is a system on one event type, handed
its batch. These are two of rocks' handlers:

```cpp
constinit const HandlerDesc k_handlers[] = {
    {.name = ".on_exit",   KV_ON(ExitRequested, on_exit), .phase = Phase::Sim},
    {.name = ".on_expire", KV_ON(Expire, on_expire),      .phase = Phase::Input},
    // ...
};

void on_expire(TickContext& ctx, Span<const Expire> expiries) {
    for (const Expire& e : expiries) ctx.destroy(e.bullet); // already gone if it hit: silently nothing
}
```

`Expire` comes from a **timer**. When `control` fires a bullet, it schedules the event
that will remove it:

```cpp
if (!b.is_null()) ctx.schedule_in(Expire{b}, kBulletLife);
```

The timer wheel delivers it at the start of the tick `kBulletLife` ticks later. If the
bullet hit a rock in the meantime, the handle no longer resolves, and the engine drops
the event before the handler sees it. A handler never needs to check whether an entity in
its batch is still alive.

## Pause and time

`engine_pause(e, true)` holds the sim. The clock keeps producing ticks, but they are
*paused ticks*: the sim tick doesn't advance, timers and triggers wait, and only systems
marked `.runs = RunWhen::Always` run. Input sampling, asset loading, the CLI and drawing
are all `Always`, so a paused game still responds. Handlers run whenever they have
events. That's why rocks quits from a handler (`on_exit`) rather than a Sim system: a quit
requested during a pause still happens.

`engine_step(e, n)` lets `n` ticks through a pause. `engine_time_scale` slows the game
down by running fewer ticks per second, never shorter ones.

## Frame and Work units

Presentation code that has to run once per drawn frame goes in `Phase::Frame`, as
`text.layout` and `ui.draw` do. Background work that should soak up spare time goes in
`Phase::Work`. Such a unit does a slice, calls `ctx.more()` if there is more to do, and
sizes its slice by `ctx.time_left()`. The frame loop gives Work whatever time is left
before the next frame is due. Neither cycle may write sim state, and `engine_init` fails
if one declares such a write. To influence the game, emit an event instead.
[kromedemo](../../examples/kromedemo/README.md) with `--work` runs a Work unit behind a
window.

## Same input, same game

Everything above has one purpose: a tick produces the same state however it was
scheduled. Units are numbered by declaration, not by when they ran. Commands, events and
timers are applied in that order, and parallel chunks merge in chunk order. So you can
check it:

```bash
build/linux-debug/bin/rocks --demo --run-seconds 30 --hash --threads 1
```

```bash
build/linux-debug/bin/rocks --demo --run-seconds 30 --hash --threads 8
```

The two runs print the same hash every tick. When a change you expected to be harmless
changes the hashes, `--hash-systems` prints the hash after every system, which points at
the system that made the difference. `--early-reads order` and `--early-reads snapshot`
are another check: they change how the schedule is built, and must not change a hash.
`--record FILE` and `--replay FILE` ([frameloop](../../modules/frameloop/README.md#using-it))
save a run's input and play it back, comparing hashes as they go.

## Driving the loop yourself

`frameloop` is the loop for a game with a window. Two other loops exist:

- **A command-line tool** uses [tool](../../modules/tool/README.md): `tool_main` runs ticks
  and Work as fast as the work allows, with no window, until a system calls `tool_finish`.
- **A test, or a program with its own loop**, calls the engine directly:
  `engine_init(e, modules, config)`, then per frame `engine_frame(e, dt)` (and
  `engine_frame_cycle` / `engine_work` if it uses those cycles), then `engine_shutdown(e)`.
  A tool with a window can keep frameloop's window and renderer and still drive frames
  itself with `frameloop_set_config` and `frameloop_step`.

## Where next

- [Modules](../concepts/modules.md): every table a module can declare, with components,
  kinds, triggers and memory.
- [Scheduling](../concepts/scheduling.md): the resource tree, the modes, and the solver's
  rules in full.
- [frameloop](../../modules/frameloop/README.md): every config field and command-line
  option.
- [testbed](../../examples/testbed/README.md): the smallest game that uses everything once.
