# The scheduler

Nov 11, 2026. How systems declare what they touch, and how the engine turns that into an
order, a set of checks and a tick. This replaces the rules that grew up in slices 1–6 one
feature at a time. It is an experiment in correctness and readability: the same games,
with fewer special cases and a schedule that means the same thing however it runs.

## What went wrong with the old one

The intent held: systems declare read and write sets, the engine derives the order and
refuses conflicts at startup. Two things drifted.

- **"What does this declaration cover?" was answered twice.** The scheduler expanded a
  kind to its columns; the dev check at access time let a kind cover its attachments and
  bags as well. Two systems writing `.npc` and `.npc.@Quest` got no edge, shared a level,
  and could attach on one store at once. The flow check (cosmetic never feeds sim) did
  not see attachments either.
- **Order was a sequence of special-case passes.** Reader after writer; `^` turns that
  around; a pair asking for both orders is put aside; declared `after`/`before`; module
  state in declaration order, before the put-aside pairs, so pairs within one module were
  never refused; writers of one resource must be ordered, unless the resource is a sink.
  Each pass depended on the ones before it.

And some promises were not kept:

- a sink's "appends land in unit order" was a mutex queue, in thread order;
- module state was "checked" by an enum nothing read: `state_as<T>()` handed every
  system a mutable reference;
- a pair that read one resource early and another as it stood was "resolved" by
  whatever path the schedule already had, which meant one of the two reads saw the
  wrong value;
- when one spelling covered a resource as current and another as early, early silently
  won;
- spawns and destroys were never checked against the declared sets;
- a `?optional` name with a typo silently dropped the order.

## The model

### Resources form a tree

Every name a system can declare is a resource. Some resources contain others:

```
kind ".npc"               the whole kind
├── ".npc.@alive"         its liveness: which slots are alive (spawn and destroy change it)
├── ".npc.Transform2"     a column
├── ".npc.Local2"
├── ".npc.@Quest"         an attachment store
└── ".npc.@props"         the property bags
"collision.state"         a module's state
"input.actions"           a published resource
"audio.commands"          an append resource
```

Declaring a resource declares everything under it. That is the one definition of
"covers", and both the schedule and the dev checks use it: the sets a system declares
are expanded once at startup, and every check after that is a lookup in the expanded
set.

### Every access has a mode

| Mode       | Spelled                     | Sees                                    | Order                          |
|------------|-----------------------------|-----------------------------------------|--------------------------------|
| read       | `reads = "x"`               | x after every writer in the phase       | after every writer of x        |
| read early | `reads = "^x"`              | x as the phase found it                 | none of its own (see below)    |
| write      | `writes = "x"`              | x, to change                            | ordered against other writers  |
| append     | `appends = "x"`             | nothing: records go to x's owner        | none                           |

One unit, one mode per resource, however it is reached. A unit whose names cover a
resource in two modes (`*.T` and `^*[A].T`, or `.npc` and `^.npc.Transform2`) is refused
at startup with both spellings named. A unit that wants some of a kind as it stands and
some early names the parts: `.npc.@alive, .npc.Actor, ^.npc.Transform2`. A filter can
exclude a component (`*[Collider2-Parent].Transform2`) to say "the rest of them". A unit
may not read early what it writes itself: its own writes would show one way and not the
other. A kind's liveness is written only with the whole kind (`writes = ".npc"`), since a
spawn writes every column.

The context reads liveness checked like anything else: `ctx.alive(h)` and `ctx.rows(kind)`
(a kind's rows for a loop, each column checked as it is taken) need a read of
`kind.@alive`; `ctx.read_kind` needs the whole kind.

The early read of a kind covers its liveness and its columns. Attachments and bags are
sparse stores the engine does not copy: they are read as they stand or not at all, and an
early read of one is refused.

### Order is solved, not accumulated

The schedule for a phase is built from one table of constraints, each with its reason:

1. a reader runs after every writer of what it reads;
2. `after` and `before` add what the declarations do not say;
3. two systems of one module that both use its state, one of them writing, run in
   declaration order.

Then it is checked, as a whole:

- two writers of one resource must have a path between them, either way round;
- no cycles;
- an early reader of an ordered kind does not come after its writer. The common case is a
  "torn" pair, a unit that reads one thing early from a writer and another as it stands:
  ordered, it would have to run both before and after. It is refused with both resources
  named; the fix is to say which value it wants, or to make the kind a snapshot, where
  the pair is fine.

None of these depends on the order they are added in. Errors name every unit and
resource involved.

`?name` in `after`/`before` is an order on a system that may not exist. It may name a
module that is not in the build; if the module is there, the system has to be too, so a
typo is an error, not a silent no-op.

### Early reads, two ways, chosen per kind

What an early read sees is fixed by the model: the phase's starting value. How the engine
delivers it is chosen kind by kind (`KindDesc::early_reads`):

- **Order** (the default): an early reader runs before every writer of what it reads
  early, so the value in the store is still the starting one. Nearly free; adds edges,
  which narrow levels and can make cycles.
- **Snapshot**: at the start of the phase the engine copies what some unit reads early
  and another writes, and early readers are handed the copy. No edges, so the reader can
  run beside the writers, and a cycle the edges would make is not there; the price is
  the copy, every phase that needs it. The copy is of the kind's liveness (the slots'
  generations) when someone spawns or destroys there, and of the columns read early: the
  early reader sees exactly the entities alive at the phase start, wherever spawns and
  destroys happen in it.

So a game pays for snapshots only where it wants what they buy, and tunes that kind by
kind. The startup log shows, for every phase with early reads, its levels as declared,
with every kind ordered (or that it would be a cycle), and with every kind a snapshot;
the profiler shows the copies (`snapshot` zone, `snapshot KB` counter).

The engine can override the per-kind choice for every kind at once
(`Engine::early_reads`, `--early-reads=order|snapshot`) to compare. Every kind a
snapshot is always a valid schedule, given the rest is; every kind ordered can be a
cycle where a kind is a snapshot to break one, and then init says so.

Because the model fixes what every unit sees, every choice must produce the same game:
the same state hash every tick. Two things make that true beyond the values:

- **Units are numbered by declaration rank, not by schedule position.** Commands apply,
  events deliver and tickets number in unit order; the schedule decides only when units
  run. A new edge or a different choice moves units between levels and changes nothing
  they produce.
- **Writers of one resource keep their order** whatever the choice, since that order
  comes from constraints that do not depend on it.

Running a replay with every kind ordered and with every kind a snapshot, and comparing
hashes, is then the test of the whole scheduler: any difference is an undeclared access,
a missing edge or a bug here.

### Module state is a resource

`collision.state` is in the tree like anything else; `SystemDesc::state` is how a system
declares it (Write, the default; Read; None). The accessor enforces it:
`state_as<T>()` is the write and needs Write; `state_as<const T>()` is the read and
needs Read or Write; a system with None gets neither. Handlers run one at a time and
always have their module's state.

### Append resources

A resource declared with an `apply` function (`ResourceDesc::apply`) takes appends. A
unit that declares `appends` on it hands records to `ctx.append(resource, record)`; they
go into the unit's own command buffer, and at the phase boundary, with the spawns and
destroys, the engine hands each to the owner's apply function, in unit order. Nothing
else touches the owner's data during the phase, so appends need no order between
themselves or with anything else. Only the owner writes an append resource; another
module declaring a write on it is told to append.

`audio.commands` is the first. A play from a system returns a voice handle at once, as
before, but the handle is a reservation numbered like a ticket; the voice behind it is
picked at the boundary, in unit order, so which voice a play gets, what it merges with
and what it steals no longer depend on which thread got the lock first.

### Smaller things

- **Commands need no declaration.** `spawn_later`, `destroy` and `destroy_all` are
  applied at the boundary, when nothing runs, so they cannot race and add no order; that
  is now the stated rule rather than an omission. Spawning and destroying *now* takes
  the table, `write_kind`, which needs a write on the kind.
- **Kind names in the context** are qualified the same way everywhere (`".rock"` is the
  module's), `spawn_later` included.
- **Ids from the context carry their engine.** `ctx.resource_id()` and `ctx.column_id()`
  return ids tagged with the engine they came from; using one with another engine is
  caught (dev builds) instead of reaching the wrong resource. Code that caches ids in
  statics keeps working with one engine and is told when it does not.
- **A failed job pool fails init** before any module starts.
- **Tables around the context.** `engine_table` and `engine_table_of` hand out the table
  as it is now. A snapshot reader that reaches a kind it reads early that way would read
  the present while the schedule assumed the past, and nothing would tell it: dev builds
  refuse it, naming the system and the kind. The engine's own lookups use
  `engine_table_now` / `engine_table_of_now`, as does a system that wants only what never
  changes (a kind's handle kind). The guard covers what runs inside a unit, its
  `parallel_for` chunks included. Running a game with every kind a snapshot in a dev
  build is the sweep that finds these.

## What stays

The runtime half was sound and is unchanged: levels with barriers on the job pool,
main-thread systems first in a pooled level, per-unit event writers and command buffers
merged in unit order, per-chunk buffers in `parallel_for`, tickets, timers, handler
waves, triggers, the paused tick.

## Costs to measure

- Snapshot copies per phase: the bytes of every early-read, written column, plus the
  liveness of their kinds. For arcfire at stress that is mostly bullets; measured below.
- Levels with every kind ordered and with every kind a snapshot: how much more runs at
  once.
- Tick time both ways, against the old scheduler.

## Later: tuning the mix by itself

Since every mix of order and snapshot kinds gives the same game, the choice is purely a
performance one, and nothing stops the engine from making it: measure the phases, try
a kind the other way, keep what is faster, and think again when the load changes (a
bullet storm makes a big kind's copy expensive; a quiet stage makes the edges cost more
than the copies). Nothing in the model is in the way. Unit numbers do not depend on the
schedule, and levels and snapshots are planned per phase from the access lists, so a
re-plan between ticks changes no result. What it needs is the measurement and a policy,
and a rule for kinds whose ordering would be a cycle (those stay snapshots).

## Results

Branch `scheduler-access-modes`, Linux, Nov 11, 2026.

**What the games had to say.** Every game's declarations went through the new rules, and
the rules found what the review said they would:

- `collision.resolve` read `*[Collider2].Transform2` and `^*[Parent+Collider2].Transform2`:
  two spellings of one column, the early one silently winning, and it moved the child
  bodies it read early. Now `*[Collider2-Parent].Transform2` as they stand,
  `^*[Parent+Collider2-Move2].Transform2` early, and a child that moves through the write.
- Arcfire's boss parts and egg shell are spawned and moved by `stage` and `egg` before the
  resolve, which wants them where the last compose put them and the bosses where the
  stage left them: the "torn" pairs the old scheduler settled by its path, with one of the
  two reads seeing the other value. Both kinds are snapshots now, the case snapshots are
  for; every kind ordered is a cycle there, and init says so when forced.
- Arcanum's `walk` read the villagers whole and, early, their positions. It now names
  what it reads of the one it faces: `.npc.@alive, .npc.Actor, .npc.@props`, with
  positions early, and asks `ctx.alive`.
- The sweep with every kind a snapshot found systems that read a kind early and reached
  its table directly: arcanum's `villagers` (liveness), arcfire's `control` (the options'
  liveness), `shots` (targets' rows and their bosses) and the pilots. The pilots (`bot`,
  and `demo`, which declared no reads at all) read the whole field, the world's `Run` and
  `Stage` and the ship's `Ship` included, none of it declared. They go through the
  context now and declare it: the bot the field early, the demo as it stands in Input.
  None of this changed a hash (the readers ran before their writers either way), which
  is the point of the guard: the hash comparison could not have found them.
- Arcfire's `.glows` declared no state and read the game's textures through it; it is a
  reader now, which costs Sim a level (15 to 16).
- 29 declarations of `audio.commands` moved from `writes` to `appends`.

**The same game whatever the choice.** State hashes compared tick for tick:

| run | ticks | as each kind says | every kind ordered | every kind a snapshot |
|---|---|---|---|---|
| arcanum `--demo` | 1,865 | 9224de52db9aac0c | the same, every tick | the same, every tick |
| rocks `--demo`, 30 s | 1,708 | | the same, every tick | the same, every tick |
| arcfire Shoreline `--demo` | 14,322 | df0a09551471e9b0 | a cycle (parts, shell) | the same, every tick |
| arcfire Orbit `--demo` | 17,911 | 5ba13cf991c250e4 | a cycle | the same, every tick |

Arcanum's hash is main's, tick for tick. Arcfire's Shoreline is main's up to tick 10,292 and
then part: at tick 10,226 a formation shot down drops a power-up and a bomb in one tick,
from two units, and their spawns now apply in rank order where they applied in run
order before, so the two items swap slots. Numbering units in run order again (a
throwaway build) gives main's Shoreline exactly, 13,793 ticks to 1bd0cb38e9544314: the
numbering is the whole of the change; the snapshots, the collision spellings and the
rest moved nothing.

**Recordings.** Arcfire's two recorded bot runs diverge on the branch (Hard at tick 1,380)
and were recorded again: Normal ends at tick 14,648, Hard at 14,769, and both replay
matching at 1 and 16 threads, as each kind says and with every kind a snapshot.

**What it costs.** Arcfire `--stress`, dev build, `balanced` power profile, so the
clocks move and differences under a tenth of a millisecond are noise.

- The copies. As the kinds say (parts and the shell), 5.1 µs a tick, under 1 KB. Every
  kind a snapshot, bullets included (the bot reads the field early): 44 µs a tick, 95 KB
  a tick on average and 356 KB at the peak, against a tick of about 1 ms.
- The levels. Sim has 22 units in 16 levels whichever way early reads go; arcfire's
  serial chain is its module state, not its early reads, so snapshots buy it nothing
  there. Arcanum's Sim is 5 levels either way.
- Against main. The stress runs part at tick 480 (the numbering), so only the first eight
  seconds are the same work: there the tick is 0.96 ms against main's 1.09 at 16 threads,
  0.64 against 0.70 at 1. Over the whole run the branch's game has half again as many
  bullets (4,935 a tick on average against 3,233), so whole-run means compare nothing.
  The access check bisects one sorted list where it scanned two, in dev builds.

**Size.** The scheduler's code (declarations to schedule, in `engine.cc`) went from 401
lines to 630, and `phase.cc` from 402 to 563, comments and blank lines left out. The
growth is the snapshots and their views, appends, the per-kind choice, the guard, and
errors that say what to do; the order itself is one table of constraints, gathered,
checked and placed by separate functions.

**Hashes after the follow-ups** (liveness named, the guard, the pilots through the
context): every demo above the same tick for tick as before them, both ways, and both
recordings matching at 1 and 16 threads both ways.

**What correctness costs.** The branch numbering units in run order (a throwaway build,
one line changed) plays arcfire's stress as main does, hash for hash (8,398 ticks
compared), so the pair isolates the scheduler and its checks. Mean tick, timed around
`run_tick` (ship builds have no profiler), over 45 s of `--stress` (about 2,700 ticks),
runs alternated, `balanced`:

| | main | branch | difference |
|---|---|---|---|
| ship, 16 threads | 1.37, 1.33 ms | 1.37, 1.32 ms | none (−0.6%) |
| ship, 1 thread | 0.98, 1.00 ms | 0.98, 0.98 ms | none |
| dev, 1 thread | 1.14, 1.19 ms | 1.17, 1.15 ms | none |
| dev, 16 threads (7 pairs) | 1.37 to 1.57 ms | 1.37 to 1.66 ms | about +0.07 ms (+5%) |

- Ship pays nothing measurable. The checks are compiled out; the snapshot copies (parts
  and shell) are 5 µs, under half a percent; the views, the deferred audio appends, the
  numbering and the honest level `.glows` added do not show.
- Dev at 16 threads is slower in six pairs of seven, by 0.02 to 0.14 ms, and not at 1
  thread, so not the per-access checks. With the profiler recording, the difference is
  8.5 µs a tick, 5.5 of them the copy. The guard (a thread-local read at every
  `engine_table` call and `parallel_for` chunk) is the suspect, but a build without it
  did not settle it: one build's runs spread 1.35 to 1.66 ms, wider than the gap. A
  `performance` profile or `perf` would.
- Snapshots where they are not needed are the real cost: every kind a snapshot is 1.43
  against 1.35 ms in ship (about +6%) and 1.55 against 1.42 in dev (about +9%), one run
  each; the copies are 44 µs and 95 KB a tick at stress. Order stays the default for
  that reason, and a kind is a snapshot where it buys parallelism or breaks a cycle.

**Not done here.** Windows and macOS have not run the branch; their recordings want
making again there. The tuning of the mix by itself is above, under Later.

