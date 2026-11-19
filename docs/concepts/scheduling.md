# Scheduling

Systems say what they touch. The engine works out the order from that, refuses anything
that would race or contradict itself at startup, and runs whatever is independent at the
same time. This page covers the rules as they stand. The reasoning and the measurements
behind them are in the design record, [docs/design/scheduler.md](../design/scheduler.md).

**Principles:** [3. Your declarations are a contract](../principles.md#3-your-declarations-are-a-contract), [1. Deterministic behavior](../principles.md#1-deterministic-behavior), [5. Fail loudly, with names](../principles.md#5-fail-loudly-with-names)

## Units and their sets

A **unit** is a system (`SystemDesc`) or a handler (`HandlerDesc`). Each one lives in one
phase and declares its access as strings in the [name language](modules.md#systems):

| field | holds | example |
|---|---|---|
| `reads` | resources read as they stand after every writer in the phase; `^x` reads the value the phase started with; event types consumed | `"*.Velocity, input.state, ^.npc.Transform2, ShotHit"` |
| `writes` | resources changed | `".ship.Transform2, .bullet"` |
| `appends` | append resources: records handed to the owner at the boundary | `"audio.commands"` |
| `emits` | event types emitted | `"RockHit, ShipHit"` |
| `after`, `before` | systems this one must follow or precede; `?name` holds only if that module is in the build | `".move"`, `"?sprite_ui.layout"` |
| `state` | the module's own state: `Write` (the default), `Read`, `None` | `StateUse::None` |

These sets are the whole contract. The scheduler orders units by them, runs units in
parallel when their sets allow it, and in dev builds asserts on any access through the
context that a unit did not declare, naming the unit and the resource. What a unit
reaches around the context (`ctx.engine`, the table directly, another module's functions)
the scheduler can't see: keep that to main-thread units, things that never change, and
functions that say they are safe. A main-thread-only API says so itself, by stopping a
dev build when a worker calls it (`engine_check_main_thread`).

## Resources form a tree

Every name a unit can declare is a resource, and declaring a resource declares
everything under it:

```
kind ".npc"               the whole kind: spawn, destroy, every column
├── ".npc.@alive"         which slots are alive
├── ".npc.Transform2"     one column
├── ".npc.@Quest"         an attachment store
└── ".npc.@props"         the property bags
"rocks.state"             a module's state (implicit; see `state`)
"input.state"             a published resource
"audio.commands"          an append resource
```

Wildcards spread over kinds. `*.Velocity` means that column in every kind that has it.
`*[Collider2].Transform2` means only the kinds that also have `Collider2`, and
`*[Collider2-Parent].Transform2` means those of them without `Parent`.

## One mode per resource

| mode | spelled | sees | ordered |
|---|---|---|---|
| read | `reads = "x"` | x after every writer in the phase | after every writer of x |
| read early | `reads = "^x"` | x as the phase found it | before the writers, or from a copy (below) |
| write | `writes = "x"` | x, to change | against every other writer of x |
| append | `appends = "x"` | nothing: records go to x's owner | not at all |

A unit has one mode per resource, however it reaches it. If two of its names cover the
same resource in two modes (`*.T` with `^*[A].T`, or `.npc` with `^.npc.Transform2`), the
unit is refused with both spellings named. A unit that wants some of a kind as it stands
and some of it early names the parts: `.npc.@alive, .npc.Actor, ^.npc.Transform2`.

## How the order is solved

For each phase the engine builds one table of constraints:

1. a reader runs after every writer of what it reads;
2. `after` and `before` add order the sets do not imply;
3. two units of one module that share its state, at least one of them writing it, keep
   declaration order.

Then it checks the whole table:

- **Two writers of one resource need a path between them.** Without one, `engine_init`
  fails and names both units and the resource. The fix is an `after` on one of them.
- **No cycles.** The message prints the loop. The fix is to move one unit to another
  phase, read the previous tick's copy (`CompPrev`), or read early.
- **No torn reads.** A unit that reads one thing early from a writer and another thing
  as it stands from the same writer would have to run both before and after it. The
  error names both resources. The fix is to say which value it means, or to make the kind
  a snapshot.

Units with no path between them share a **level**. A level's units run at once on the
job pool, with a unit pinned to the main thread (`.main_thread = true`) running first;
the next level waits for all of them. Ties keep module order, then declaration order.

## Early reads: order or snapshot

What an early read sees is fixed: the phase's starting value. How the engine delivers it
is chosen per kind, with `KindDesc::early_reads`:

- **`EarlyReads::Order`** (the default) runs the reader before every writer. It costs
  almost nothing, but it adds edges, which narrow levels and can close a cycle.
- **`EarlyReads::Snapshot`** copies what is read early and written at the start of the
  phase, then hands the reader the copy. It adds no edges, so the reader can run beside
  the writers. The copy costs something every phase it is needed; the profiler shows it
  as the `snapshot` zone and the `snapshot KB` counter.

The game is the same either way. `--early-reads order|snapshot` forces one choice for
every kind, and the state hash must not change; a difference means an undeclared access
or a missing edge. A snapshot reader must reach the kind through the context (`ctx.view`,
`ctx.read`, `ctx.rows`, `ctx.read_kind`). Dev builds refuse `engine_table` there, because
it hands out the table as it is now.

## Results don't depend on the schedule

Units are numbered by declaration rank, not by schedule position. Commands apply, events
deliver, appends reach their owner and tickets are numbered in unit order, and
`parallel_for` chunks merge in chunk order. The schedule only decides *when* a unit runs,
never what comes out. As a result, `--threads 1` and `--threads 16` hash the same, and
adding an edge or flipping a kind to a snapshot moves no hash.

## Reading the schedule

`engine_init` logs every edge with its reason, then the order of each phase with `|`
between levels. This is the start of rocks' Sim phase:

```
engine: phase sim:
engine:   collision.resolve after rocks.move: rocks.ship.Transform2 and 2 more
engine:   rocks.wrap after rocks.move: declared
engine:   rocks.game after rocks.control: rocks.state, in declaration order
...
engine: order sim     collision.on_sim_reset, frameloop.on_sim_reset, rocks.control | rocks.move, rocks.game | rocks.wrap, rocks.on_exit | collision.resolve, rocks.on_asset_failed | collision.compose, collision.broadphase
```

Each edge's reason is a resource (a read after a write), `declared` (`after` or
`before`), or a module's state kept in declaration order.

## See also

- [Cycles](cycles.md): which phases run when, and what Frame and Work units may touch.
- [Modules](modules.md): the tables in full, and what happens at each boundary of a tick.
- [Frame loops and scheduling](../guides/frame-loop-and-scheduling.md): a guided walk
  through all of this with rocks.
- `korvid/core/phase.h`: `SystemDesc`, `HandlerDesc`, `TickContext`, `parallel_for`.
