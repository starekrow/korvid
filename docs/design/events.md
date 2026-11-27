# Events in the schedule

Nov 11, 2026. Branch `handlers-in-schedule`, an exploration: handlers become systems,
and events become resources the schedule orders like any other. Written first; the
code follows it.

## Why

The scheduler orders systems by what they declare and checks the order as a whole
(`docs/design/scheduler.md`). Handlers sit outside that:

- **A second scheduler.** At every phase boundary the bus delivers what is pending, the
  handlers with new events run in list order, their commands apply, and what they
  emitted is delivered again, in waves, up to a cap of four. When a chain of events
  (overlap, hit, kill, pickup) lands depends on how deep it is and which phase started
  it. Past the cap, the rest waits for the next boundary, after the next phase's systems
  have run, and the only sign is a profiler mark.
- **`after` that does not mean after.** Handlers order within a wave. A handler declared
  after another runs after it only when both have events in the same wave; the event one
  handler emits for the other arrives a wave later whatever the declarations say.
- **Undeclared.** Half of arcfire's handlers declare nothing: they act through the module
  state, which a handler always holds, and through the engine directly. Systems read the
  tick's delivered events (`ctx.events->read<E>()`) with no declaration at all, and
  nobody declares what they emit, so nothing can order a reader after an emitter.
- **Timing nobody wrote.** An event names a handle; the entity dies at a boundary; the
  bus drops the event there, silently in ship builds. A handler sees whatever state the
  boundary's earlier waves left. Where a hold, a quit or a reset lands is a matter of
  which boundary handled what.

Each of these has cost a bug in a game. None of them is checked.

## The model

### An event type is a stream

Every event type is a resource in the tree, a **stream**, named by the type
(`ShotHit`). Emitting appends to it; consuming reads it:

| | spelled | means |
|---|---|---|
| emit | `emits = "ShotHit, Kill"` | appends to the stream; no order of its own |
| consume | `reads = "ShotHit"` | the events emitted since this unit last ran, up to now: after every emitter of it in the phase |
| consume early | `reads = "^ShotHit"` | the same, but only up to the phase start: emitters in this phase are next time's |

A consumer runs after every emitter of the stream in its phase, like any reader after
any writer, so a chain within a phase resolves in one pass in data order: the
broadphase emits overlaps, the overlap system turns them into hits, the hit system into
kills, all in Sim, all before PostSim. A loop (A consumes what B emits and B what A
emits) is a cycle and refused at startup; the fix is an early read, which says "what
came before this phase" and needs no order, so the loop goes round once a phase.

An early read of a stream needs no copy and no order in either early-read mode: the
batch is defined by where it ends, the phase start.

### Every consumer sees every event once

A consumer's batch is every event of the type emitted after its last run and before
this one. Events emitted in a later phase (or at a later boundary, or outside the
schedule) reach it on its next run, next tick: data flows forward in the tick, and back
only across a tick, as for any resource. A unit that did not run (a paused tick) sees,
when it does, everything since it last ran.

Two consumers of one type see the same events in the same order, less what each one's
run boundaries put in another batch.

### The order of a batch does not depend on the schedule

Events land in **segments**, one for each place in the tick events come from: the tick
start (timers, events from outside the schedule), each phase, and each boundary (the
engine's `Spawned`, at the commands). Within a phase's segment, events go by the
emitting unit's rank, then the order it emitted them (its `parallel_for` chunks in chunk
order), never by when it ran. A consumer's batch is the segments since its last run, in
tick order.

A phase's segment of one type is complete when its last emitter in the phase has run,
which is before any consumer of it in the phase runs. It is merged once, at the start of
the first level that holds a consumer, in rank order. So a batch is the same however the
levels fell, whichever way early reads are served: the property the scheduler's hash
comparison rests on carries over to events.

### Handlers become systems

A handler is a system with a phase that consumes one stream and gets its batch as a span:

```cpp
{.name = ".on_shot_hit", KV_ON(ShotHit, on_shot_hit), .phase = Phase::Sim,
 .reads = "...", .writes = "...", .emits = "Kill"}
```

It is ordered by the same constraints as every system: after the emitters of what it
consumes and of what it reads, before the readers of what it writes and emits, in its
module's state order otherwise. `after` and `before` mean what they mean for systems.
There are no waves, no wave cap and no separate handler list. A handler runs on every
tick, paused ones included (as handlers always have), and with an empty batch only when
it says it wants to (`RunWhen`); by default it is not called on an empty batch.

A system that reads a stream outside a handler reads its batch through the context
(`ctx.received<E>()`), declared like any read.

### What stays

- Events are plain data with optional schemas, emitted into the unit's writer.
- `EvNextTick` types: a consumer in the same tick never sees one; they land in the next
  tick's start segment.
- The stale-handle drop: an event naming a handle that no longer resolves is dropped
  when a batch is made, for each consumer, counted, and warned
  about once a type in dev builds unless the type says `EvStaleOk`.
- Timers fire at the tick start, in ticket order, into the start segment.
- Triggers emit `StateEdge` at the end of the tick, into the next tick's start segment.

## Two ways to emit

- **`ctx.emit`, from a unit:** into the unit's writer, declared in `emits` (dev builds
  check), ordered as above.
- **The engine's writer (`engine_emit`, today `e.events.emit`):** for what happens outside
  the schedule (the window's input, the frame loop's quit, the asset pumps' results, the
  engine's `Spawned` at the commands, `SimReset`). Those events land in the next segment:
  the boundary after the phase, or the tick start. No consumer in the phase that emitted
  them sees them, so they need no order. From inside a unit they are allowed only on the
  main thread (dev builds check). A library call that reports a fact as it goes (an asset
  load failing inside a level build) uses this, so the caller need not declare what a
  library might say.

## What changes for a game

- Every handler gets a phase. It runs in that phase when its batch is not empty, paused
  ticks included, as handlers do now.
- Emitters declare `emits`. Readers of a stream outside a handler declare it in `reads`
  and use `ctx.received<E>()`; `ctx.events->read<E>()` (the whole tick's batch) is for code outside the schedule only.
- A chain within a phase is resolved in that phase, and its commands land together at
  the phase's end. A hit handler that destroys an enemy and emits `Kill` for it: the kill
  handler, later in the phase, sees the event, the enemy still in its ktable (the destroy
  is pending). Today the commands apply between waves, so the `Kill` names a dead handle
  and is dropped: arcfire's "the bus drops events whose handles are stale" lesson is this.
- Timing moves a little. A chain's commands land at its phase's end, not between its
  steps; `Spawned` for a spawn made in a chain comes a phase later. Hashes and recordings
  change; the demos must still hash alike at 1 and 16 threads and with every ktable a
  snapshot.

## Decisions taken from the inventory

- **Phase per handler.** An old boundary handler ran after one phase's systems and before
  the next's. That has two faithful places, and the declarations decide which:
  - **the end of the phase that emits the event**, after its emitter, when what the
    handler writes must be the next phase's start: arcanum's level build (PreSim, after
    `level.run`), whose players and villagers Sim's early readers must see as Sim found
    them;
  - **the start of the next phase**, reading early (`^`) what that phase's systems write,
    when systems earlier in the emitting phase read what the handler writes: arcfire's
    and rocks' hit chains go in PostSim (the resolve, early in Sim, reads what the hits
    change; in Sim that is a cycle).
  The scheduler refuses the wrong one with the cycle or the contradiction it makes.

| events | from | handlers go in |
|---|---|---|
| OverlapBegin and the hit chains (ShotHit, BulletHit, Rammed, Graze, ShellHit, Pickup, Kill, ShipDied; RockHit, ShipHit) | collision.broadphase, Sim; then the hit handlers | PostSim, reads early |
| BeamHit, Bombed, StageEnd | .beams, .bomb, .stage (Sim) | PostSim, with the hits |
| StateEdge, timers, next-tick types | end of tick, tick start | Input (the tick after, as before) |
| Spawned | the commands, every boundary | PostSim |
| AnimFinished | anim.play, PostSim | Input of the tick after (as before) |
| LevelBuild, LevelLoading, SpuiEvent, SpuiCovered | level.run, sprite_ui.update, PreSim | arcanum: PreSim, after them; arcfire's flow: Sim |
| ExitRequested | frameloop.exit, PreSim | Sim |
| input events | the engine's writer | Input |

- **Ranks.** A module's handlers rank after its systems, in declaration order, so module
  state orders them after its systems in their phase unless something says otherwise.
- **`after` among handlers.** Most of arcfire's (`on_bomb` after `on_shot_hit`,
  `on_stage_end` after `on_ship_died`, `on_pickup` after `on_kill`) ordered handlers whose
  events came in different waves, so they never applied: `StageEnd` was handled in wave
  0, `ShipDied` in wave 2. They are gone; the events order the chain, and module state
  (declaration order) the rest. What PostSim systems an early handler must precede is
  said where the scheduler asks (`on_pickup`, `on_ship_died` before `anim.play`), and
  arcanum's level build after `tilemap.pump`.
- **Waves, the cap, `ctx.wave`, `Engine::handlers` and `waves_capped` go.** The boundary
  keeps the timers (tick start) and the commands; delivery is the segment merge.
- **Stale drop per batch.** Each consumer's batch drops what names the dead when it is
  made. Within a phase nothing dies (destroys are commands), so the drop only ever meets
  events that crossed a boundary.

## Open questions

- A handler that wants to run at more than one point in a tick (a quit that must land on
  the same tick it is asked, wherever that was): none in the inventory needs it; one
  phase is the rule until one does.
- `engine_sim_reset` empties the sim from inside a phase now, not at a boundary: the units
  after it in the phase see the fresh sim. Arcfire calls it from PreSim on a held tick,
  where nothing sim runs after it. Kept, and documented.
- Handlers still reach the engine directly (`engine_hold`, `ui_*`, `engine_request_quit`,
  `platform_io`). Those are effects outside the schedule, as before; this branch does not
  try to declare them.

## Inventory

Seven modules have handlers: collision (1), frameloop (1), rocks (12), arcfire (20),
arcanum (5), testbed (5), cpukiller (1), window (8, all logging or quitting).
Events come from systems (`ctx.emit`, `cc.emit` in chunks), handlers (chains), timers, the
engine's writer (input, pumps, `frameloop.exit`, `Spawned`, `SimReset`) and the triggers
(`StateEdge`). Systems that read the tick's batch directly: rocks `.pause` (KeyDown,
PadButtonDown) and `.camera` (GameOver, ShipHit), testbed `.sim` (KeyDown, KeyUp,
AssetReady, AssetFailed, ExitRequested), stones (ExitRequested), `frameloop.exit`
(CloseRequested).

The deepest chain is arcfire's hit chain: the broadphase's overlaps, then the hits, then
kills and deaths, then the spawns of what the kills drop: four waves, the cap, and it
stops there only because the last handler ignores what it sees.

## Migration

1. Core: streams in the resource tree; `emits`; batches with cursors and segments; the
   engine's writer as the outside path; handlers as systems with a phase; waves gone.
   Tests rewritten for the new delivery (handler_test, event_test, reset_test,
   trigger_test).
2. Engine modules: collision, frameloop, anim, asset, level, ui, audio, platform_hid,
   touchpad declare `emits` or use the engine's writer.
3. Games: every handler gets a phase, every emitter `emits`, every batch reader a declared
   read. The scheduler's checks say what is missing.
4. Hashes and recordings again; the comparison both ways; the guard sweep; the costs.

## Results

Branch `handlers-in-schedule`, Linux, Nov 11, 2026.

**What it took.** Core: the streams, the segments, the cursors, the engine's writer as
the outside path, handlers built into their phases, waves and `Engine::handlers` gone.
Engine modules declare what they emit (collision, anim, audio, level, ui, the frame loop)
and two handlers got phases. Games: every handler a phase, every emitter `emits`, every
reader of the tick's events a declared read. Two things in the scheduler itself turned up
on the way:

- Module state skipped a pair that only an ordered ktable's early read put in order, and
  the writers check (rightly) wanted an order that holds with every ktable a snapshot. State
  edges are now judged against the mode-independent graph and oriented along the order
  that includes the ktables' early edges, so they never contradict one.
- Handler-to-handler `after`s that never applied (above).

**What the checks found.** The guard sweep (dev, every ktable a snapshot) caught the hit
handlers reaching ktables directly once they read early: `hurt`'s handle kinds,
`bullet_row`, `on_bomb`'s liveness walk. Handle kinds now come from `engine_ktable_now`
(they never change), liveness from `ctx.rows` with `@alive` declared.

**The same game, still.** Tick for tick, at rate 4:

| run | ticks | hash | every ktable a snapshot | 1 thread |
|---|---|---|---|---|
| arcanum `--demo` | 1,865 | 9224de52db9aac0c (main's) | the same | the same |
| arcfire Shoreline `--demo` | 14,172 | a85e2f7fa02412e5 | the same | the same |
| arcfire Orbit `--demo` | 17,333 | 1b52151fc10a42fd | the same | the same |
| rocks `--demo` | 19,189 compared | | the same | the same |

Arcanum's hash did not move at all. Arcfire's and rocks' did: the chains now settle in
one phase with their commands at its end (where the waves applied them between handlers),
`Spawned` arrives a phase later for spawns made in a chain, and the handler `after`s that
never applied are gone. Both arcfire recordings were made again (14,648 and 14,769 ticks;
the stage ends where it did) and replay matching at 1 and 16 threads, per ktable and as
snapshots. 25 suites pass in dev, debug and ship.

**What it costs.** Dev build, `--stress`, 25 s traced twice each, the same game (the
summaries match): the boundary and the new tick start take 17 µs a tick against main's 23
for the waves; the tick is 1.09 ms against main's 1.01 and 1.07, which is main's own
spread on `balanced`.

**What is left open.**
- Handlers still reach the engine directly (`engine_hold`, `ui_*`, `engine_request_quit`,
  `platform_io`): outside the schedule, as before.
- The phase of a handler is the author's choice between two places; nothing infers it.
  A default could ("after the emitter, unless that is a cycle"), but a rule that moves
  code between phases by itself is the kind of thing this branch set out to remove.
- Windows and macOS have not run it.

## Later: the stale check moved to the level's start

Nov 26, 2026, branch `perf/concurrency-numbers`. "For each consumer as its batch is made"
above meant as the consumer started, on whatever thread it started on. Whether an entity
is alive is state no unit declares, so nothing ordered that read against a unit of the
same level holding the ktable's write and destroying rows directly. `tests/stale_order_test.cc`
(20,000 entities, an event each, a destroyer and a counting handler in one level, 20
ticks): with no workers a level runs in rank order, the destroyer first, and the handler
was handed 0 of 400,000 events; with workers main-thread units run first, and it was
handed all of them; with `any_thread` it was handed 121,339 to 234,285, a different number
every run, and the read raced the destroy. The 1-against-16 hash comparison is the check
that should have caught the first of these, and no game had the pattern.

Batches are now made for every unit of a level before any of them runs (`prepare_unit`,
`phase.cc`), on the calling thread, into scratch held for the level: every consumer sees
who was alive when its level began. All three cases above are handed 400,000 and hash the
same. A handler beside a destroyer is now always handed the events, where on one thread it
was not; rocks' hashes and arcfire's two recordings did not move.
