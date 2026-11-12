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
  when a batch is made, for each consumer as its batch is made, counted, and warned
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
  handler, later in the phase, sees the event, the enemy still in its table (the destroy
  is pending). Today the commands apply between waves, so the `Kill` names a dead handle
  and is dropped: arcfire's "the bus drops events whose handles are stale" lesson is this.
- Timing moves. `on_spawned` sees spawns in the phase it is placed in, not at the next
  boundary; arcfire's hit chain runs inside Sim instead of at the PostSim boundary.
  Hashes and recordings change; the demos must still hash alike at 1 and 16 threads and
  with every kind a snapshot.

## Decisions taken from the inventory

- **Phase per handler, chosen where its event comes from**:

| events | from | handlers go in |
|---|---|---|
| OverlapBegin | collision.broadphase, Sim | Sim (after the broadphase, by the stream) |
| arcfire hits (ShotHit, BulletHit, Rammed, Graze, ShellHit, Pickup, Kill, ShipDied) | on_overlap, the hit handlers | Sim |
| BeamHit, Bombed, StageEnd | .beams, .bomb, .stage (Sim) | Sim |
| rocks RockHit, ShipHit | on_overlap | Sim |
| StateEdge | triggers, end of tick | Sim (the tick after, as now) |
| timers (SpawnMember, StageNext, RunOver, Expire, SpawnWave, Restart, Blink*) | tick start | Sim, or PreSim for flow (StageNext, RunOver) |
| Spawned | the commands, every boundary | PostSim (shadows, flames, tickets) |
| AnimFinished | anim.play, PostSim | PostSim of the tick after |
| UiEvent, UiCovered | ui.update, PreSim | PreSim, after ui.update |
| LevelBuild, LevelLoading | level.run, PreSim | PreSim, after level.run |
| ExitRequested, AssetFailed, SimReset, input events | the engine's writer | Input or PreSim (the next tick start, or the boundary) |

- **Ranks.** A module's handlers rank after its systems, in declaration order, so module
  state orders them after its systems in their phase unless something says otherwise.
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
arcanum (5), testbed (5), cpukiller (1), example_window (8, all logging or quitting).
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

(When it runs.)
