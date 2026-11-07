# Engine Slice 2 — Dev PRD

Nov 6, 2026 · @starekrow

## Goal and exit criteria

Slice 1 proved the loop with one entity kind, one system and a handful of hand-written special cases. Slice 2 is about day 2 of a real game: several kinds of entity, components the game defines, systems that have to run in an order, things that happen because other things happened, and memory that has to fit. The engine's answer to each is data: schemas, read/write sets, subscriptions, budgets. The game programmer declares; the engine stores, schedules, delivers and accounts.

Nothing in this slice touches the renderer's inner surface. The renderer keeps consuming the `SpriteDelta` stream, the `render/` headers do not change, and rendering performance is a later slice.

It is done when:

- The example game declares its own components and entity kinds in its `ModuleDesc`, and the engine stores them. No engine source names a game type.
- The schedule within a phase is derived from declared access, not from list order. A pair of systems that conflicts is rejected by `engine_init` with both names and the resource in the message.
- A cascade of at least three events plays out within one tick. Scheduled events fire on their tick. Overlap and state edges arrive as events without any system polling for them.
- Two runs of the demo with the same seed produce identical per-tick state hashes, at 1 thread and at N threads.
- The game runs inside a declared budget. A hard limit fails with the heap and module named. The dev leak report at exit is empty.
- Validation stays clean, the Vulkan-header grep from slice 1 still passes, and `git diff --stat` on `modules/render` and `modules/renderer-vulkan` is empty except for bug fixes.

## Data layer

**Component schemas are registration data.** `ModuleDesc` gains a `components` table, one `ComponentDesc` per type: name, size, alignment, a field table (name, scalar type, offset, count) and flags.

- `sim`: authoritative state. Hashed, snapshotted, visible to triggers. The default.
- `cosmetic`: presentation-only state. Never hashed; can never be read by a `sim` system (checked at registration).
- `prev`: the engine keeps a previous-tick copy, written at the top of PreSim. `Transform2` declares it; `SpriteWorld::begin_tick` goes away.
- `KV_COMPONENT(T, fields...)` builds the row. The field table is the reflection the describe functions, the hash, the trace and the later CLI read.

**Entity kinds are tables.** A kind is a named, fixed set of components plus a capacity, declared in the module's `kinds` table. Every entity of a kind has every component of that kind; there is no runtime add or remove. Each kind is one pool with one SoA column per component, created by `engine_init` on reserved pages, and one registered handle kind, so `kv_handle_describe` on any entity prints its kind name and its field values from the schema.

- Capacity is a budget: the table is reserved at init and committed as it fills. A full table fails the spawn and logs the kind; it never grows silently.
- Columns are resources. `rocks.bullet.Transform2` is the column; `*.Transform2` names that column in every kind that has one. Systems declare reads and writes in these terms and the scheduler expands wildcards at init.
- `SpriteWorld` becomes one kind declared by the sprite module, `sprite.entity`, and keeps its public accessors. The extract walks `*.Transform2` and `*.Sprite` across every kind, so game kinds render with no extra code.
- Iteration order within a table is slot order. Across tables it is kind registration order. Both are fixed run to run.

**Systems reach data through the context.** `ctx.read<T>(resource)` returns `Span<const T>`, `ctx.write<T>(resource)` returns `Span<T>`, and `ctx.view<A, B>()` yields, per kind that has both, the matching spans and the kind's live mask. Type is checked against the schema by type key. In dev builds a request for a resource outside the system's declared set asserts with the system and resource named. This is the enforcement the slice 1 PRD stubbed, and it closes the hole where a system reaches another module's state through an accessor function.

**Spawn and destroy.**

- A system that holds the write on a kind spawns directly: `ctx.spawn(kind, init)` returns the handle at once, from the pool's FIFO freelist, so the result is deterministic because the writer is exclusive within its phase.
- Everyone else, and every job inside a parallel-for, issues a command. Commands are per job chunk and are applied in chunk order at the phase boundary. A command spawn has no handle until then; a `Spawned` event carries it if the issuer asked.
- Destroy takes effect at the phase boundary. Readers in the same phase still see the entity; events that carry its handle afterwards are dropped silently (see Events).
- Stale handles return null from every lookup, as in slice 1.

**Change tracking.** Each `sim` column carries a version that bumps once per run of a system that holds its write. Components with a `prev` copy additionally get per-row change detection by comparison, which is what the extract and the state triggers consume. There is no per-write tracking; a `Span<T>` is a plain span.

**Hashing and snapshots.**

- `--hash` prints, per tick, a hash over every `sim` column in kind order, and in dev builds one hash after every system so drift is located to the system that introduced it.
- `world_snapshot` / `world_restore` copy every table in one pass. Rewind and replay are later; the round trip is tested now so pools stay snapshot-shaped.

## Scheduler

**Names become ids at init.** Resource and system names are resolved once; `SystemDesc` keeps strings, the schedule holds ids. Unknown names fail init, as now.

**Phases stay fixed; order inside a phase is derived.** Within a phase the systems form a DAG:

- a system that reads R runs after every system in the phase that writes R;
- `after` and `before` lists of system names add edges by hand, for writer-after-writer and for anything the sets do not express;
- two writers of one resource in one phase with no edge between them is an error, as is a cycle. The message names the path, in the kormake style.
- Ties break by module order then declaration order, so the schedule is deterministic and `engine_init` logs it with the reason for each edge.

**Handlers schedule under the same rules.** Event handlers (see Events) declare reads and writes, run at phase boundaries in waves, and are scheduled inside a wave exactly like systems inside a phase.

**Job system.**

- N workers, default cores minus one, `--threads N` to override, `--threads 1` for a serial run that must match bit for bit.
- Per-thread scratch arena, reset per job; the thread-local owner is set per job so attribution follows the schedule, as the allocator design requires.
- `parallel_for(span, grain, fn)` over a column or view. Emit buffers and command buffers are per chunk and merge in chunk order, so the merged order never depends on which thread ran a chunk.
- Reductions merge in chunk order too; `parallel_reduce` is provided so nobody writes a racy one.
- Systems with no edge between them run concurrently inside a phase. A system that declares nothing it needs is the bug the checked spans catch.

**Attribution** follows the schedule into jobs: a bare `mem_alloc` inside a parallel-for lands on the right module.

## Events

Slice 1 has typed per-tick buffers delivered at phase boundaries and read in batches. Slice 2 adds everything the requirements doc says events do, except compound and script triggers.

**Event schemas.** `EventTypeDesc` gains the same field table as components. The engine then knows which fields are handles, so it can drop events whose handles are stale, print any event in the trace, and describe it from a tool.

**Handlers and cascades.**

- A `HandlerDesc` names an event type, a function taking `Span<const E>`, and reads and writes. Subscriptions are the table; a handler is referenced by its id in the table, never by a function pointer in sim state.
- At each phase boundary: deliver, run the handlers whose types have events, deliver what they emitted, run again. Waves are capped (default 4). Past the cap, what remains rolls into the next tick and a counter says so.
- An event type may declare `next_tick` delivery instead of same-tick waves. The demo uses both, and the open question on default latency is answered from measurements rather than argument.
- Several events on one entity in a batch are the consumer's problem, by rule. Delivery order within a type is emit order after the deterministic merge.

**Scheduled events.** `ctx.schedule(event, tick)` puts the event in a timer wheel keyed by tick. It is delivered at the first boundary of that tick. Scheduling returns a handle; `cancel` is P1. Thousands of pending timers are expected, so expiry is O(1) per tick plus the events due.

**Triggers are indexed, not polled.**

- **Spatial:** a `Collider2` component (circle or AABB, layer mask) in any kind opts into a uniform-grid broadphase. Per tick the engine diffs the overlap pair set against the previous tick's and emits `OverlapBegin` and `OverlapEnd`, pairs in canonical order (lower handle first). This is the day-2 need: bullets hit rocks and nobody loops over rocks.
- **Time:** the wheel above.
- **State:** a `TriggerDesc` names `kind.Component.field` and an edge (`changed`, `crossed_below(value)`, `crossed_above(value)`), using the schema to read the field. It emits `StateEdge{entity, trigger}`. Requires a `prev` copy or a column version; init rejects a trigger on a field that has neither.
- **Compound** dataflow nodes and **script-predicate** triggers are out of scope.

**The event log is the trace.** Dev builds keep a ring of delivered events per tick with the emitting system or handler id. `--trace-events` prints it through the schemas; the ring is what the later CLI and replay read. Paired with `--hash` it answers "what changed, and who did it" without a debugger.

## Memory

Steps 1 to 4 of the allocator design are done. Slice 2 lands the parts the game programmer meets.

- **Budgets are data.** The root supplies a heap table in an `EngineConfig` struct handed to `engine_init` beside the module list: heap name, soft and hard limit, and module to heap assignment. Modules keep declaring only what they need. The example game's `main` declares a `game` heap and the game lives inside it. Kormake may generate the struct later; its shape does not change.
- **Hard limits fail.** An allocation past a hard limit returns null and logs the heap, the module and the category once. A soft limit warns once, as today. No global OOM, ever.
- **Kind capacities appear in the report** as their owning module's memory, by category, so "where did 40 MB go" is answered by `mem_report`.
- **Size-class small allocator** (design step 5): segregated freelists, thread-local caches refilled in batches, remote frees per page. It sits behind `mem_general()`; nothing above notices. Accounting moves to page granularity.
- **Dev checks:** guard pages on large blocks and call-site capture behind `KV_DEV`, both off in ship. The existing fill, poison, canary and owner checks stay.
- **Leak report at exit by module and category** stays the acceptance gate; the game module must come up empty.

The game-facing surface is the C++ equivalent of the C ABI the requirements describe: the module's arena for state that lives as long as the module, `mem_general()` attributed to the module for anything growable, `mem_scratch()` with `MemScope` for tick temporaries, and entity storage the game never allocates itself. The C ABI itself is a later slice; nothing here may make it harder.

## Example game: rocks

An asteroids-like, next to `hello`, as its own module with its own `modules.gen.cc`. `hello` stays as the smoke test.

**Kinds and components.** `rocks.ship` (1), `rocks.bullet` (4096), `rocks.rock` (1024, three sizes), `rocks.debris` (8192, `cosmetic`). Game-defined components: `Velocity`, `Lifetime`, `Health`, `RockSize`. Engine components: `Transform2`, `Sprite`, `Collider2`. A `Score` resource owned by the game.

**Systems.** `control` (Input state to ship Velocity), `move` (`*.Velocity` read, `*.Transform2` write, a parallel-for), `wrap` (after `move`, by edge), `spawner` (reacts to wave timers), `debris_fade` (cosmetic only). `move` and `debris_fade` must be allowed to run concurrently by the scheduler; `wrap` must not.

**Handlers and triggers.**

- `OverlapBegin` bullet × rock → handler emits `RockHit`, destroys the bullet.
- `RockHit` → handler splits the rock into two smaller ones (direct spawn, it holds the write), spawns debris by command, emits `ScoreChanged`. Three waves within one tick.
- `OverlapBegin` ship × rock → `ShipHit` → `Health` decrement.
- State trigger: `rocks.ship.Health.hp crossed_below(1)` → `StateEdge` → handler emits `GameOver`, schedules `Restart` 120 ticks out.
- Bullet expiry is a scheduled `Expire` event per bullet, so the wheel carries thousands of timers.
- Wave spawns are scheduled events; `ScoreChanged` opts into `next_tick` delivery to exercise that path.

**Flags.** `--demo` drives the ship with a seeded script so a run has no input and is reproducible. `--threads N`, `--hash`, `--trace-events`, `--run-seconds` from the frame loop.

**Acceptance tests:**

- [ ] `rocks --demo --run-seconds 30 --hash` prints identical hashes on two runs, and with `--threads 1` and `--threads 8`.
- [ ] Swapping the `move` and `wrap` declaration order changes nothing: the schedule is derived, and the log shows the edge.
- [ ] Adding a second writer of `*.Transform2` in Sim without an `after` fails init with both system names and the resource.
- [ ] A system that calls `ctx.write` on a resource it did not declare asserts in dev builds with the system and resource named.
- [ ] A bullet hitting two rocks in one tick is destroyed once; the second `RockHit` arrives with a stale bullet handle and is dropped silently, counted in the trace.
- [ ] The `RockHit` cascade is three waves deep in one tick; raising the wave cap to 1 rolls the rest into the next tick and the counter reports it.
- [ ] 2000 pending `Expire` timers cost no measurable per-tick time beyond the ones due.
- [ ] `OverlapEnd` arrives the tick after two rocks separate; no system reads collider data directly.
- [ ] A `cosmetic` component read from a `sim` system is rejected at init.
- [ ] Filling `rocks.bullet` fails the spawn with a log line naming the kind; nothing grows.
- [ ] A 1 MB hard limit on the `game` heap fails with heap, module and category named; the game keeps running.
- [ ] `world_snapshot` then `world_restore` yields the same hash as the tick it was taken on.
- [ ] Leak report at exit is empty for `rocks`; `kv_handle_describe` on a rock prints its kind and fields.
- [ ] `hello` still runs unchanged except for the `SpriteWorld` migration, and the renderer modules show no diff.

## Requirements

P0 ships in slice 2, P1 should, P2 is designed for but comes later.

| ID | Requirement | Priority |
| --- | --- | --- |
| R1 | Component schemas in `ModuleDesc` with field tables and `sim`, `cosmetic`, `prev` flags | P0 |
| R2 | Entity kinds as fixed-component tables with declared capacity, one handle kind each, SoA columns on reserved pages | P0 |
| R3 | Columns are resources; wildcard `*.Component` expands at init | P0 |
| R4 | Typed, access-checked spans and views through `TickContext`; undeclared access asserts in dev | P0 |
| R5 | Direct spawn for the exclusive writer; command spawn and destroy from everywhere else, applied in chunk order at the phase boundary | P0 |
| R6 | `SpriteWorld` migrated onto a kind; extract walks every kind with `Transform2` and `Sprite`; render headers unchanged | P0 |
| R7 | Per-tick and per-system state hash over `sim` columns, `--hash` | P0 |
| R8 | Resource and system names resolved to ids at init | P0 |
| R9 | In-phase DAG from read/write sets plus `after`/`before`; unordered double writers and cycles rejected with the path named | P0 |
| R10 | Job system with per-thread scratch, per-chunk emit and command buffers, `parallel_for` and `parallel_reduce` with chunk-order merge | P0 |
| R11 | Independent systems run concurrently; `--threads 1` matches `--threads N` | P0 |
| R12 | Event schemas; stale-handle events dropped silently and counted | P0 |
| R13 | Handler tables, same-tick waves with a cap, per-type `next_tick` delivery | P0 |
| R14 | Scheduled events on a timer wheel | P0 |
| R15 | Uniform-grid broadphase over `Collider2` with `OverlapBegin`/`OverlapEnd` by pair-set diff | P0 |
| R16 | State triggers on schema fields: `changed`, `crossed_below`, `crossed_above` | P0 |
| R17 | Heap table in `EngineConfig` with soft and hard limits; hard limits fail with heap, module and category named | P0 |
| R18 | Kind storage attributed and visible in `mem_report` | P0 |
| R19 | Example game `rocks` with the acceptance list above | P0 |
| R20 | Event trace ring with emitter ids, `--trace-events` | P1 |
| R21 | `world_snapshot` / `world_restore` round trip | P1 |
| R22 | Cancel for scheduled events | P1 |
| R23 | Size-class small allocator behind `mem_general()` | P1 |
| R24 | Guard pages and call-site capture behind `KV_DEV` | P1 |
| R25 | Column versions queryable by systems (cheap "did anything change" skips) | P1 |
| R26 | Compound triggers as dataflow nodes | P2 |
| R27 | Runtime add and remove of components on an entity | P2 |
| R28 | Phases as declared data rather than a fixed enum | P2 |

## Out of scope for slice 2

- Renderer work of any kind: persistent GPU sprite table, compute sort and cull, indirect draws, the material parameter block, fence-tracked swapchain recreation, splitting `renderer.cc`
- Transform hierarchy, render-rate entities, simulation LOD tiers
- The C ABI and any game-side language
- CLI and line protocol, rewind and replay (snapshot only), live import
- Real io_uring behind `IoQueue`, cook pipeline, pack files
- Gamepad, Windows, audio, physics middleware
- Compound and script-predicate triggers
- Kormake (its own PRD); `build.ninja` and the hand-written module lists stay

## Open questions, risks and milestones

**Open questions**

- Fixed component sets per kind is the bet. If `rocks` wants to add a component to a live entity, R27 moves up; the PRD records that it did not.
- Default event latency: same-tick waves or next-tick. Decided after the demo has run both.
- Whether a command spawn needs the `Spawned` event at all, or whether every spawner in practice holds the write.
- Whether the four fixed phases survive once the in-phase DAG exists, or R28 becomes necessary early.
- Default worker count, and whether the main thread participates in parallel-for.

**Risks**

| Risk | Effect | Mitigation |
| --- | --- | --- |
| Determinism breaks under threads | Hash mismatch nobody can find | Per-chunk buffers merged in chunk order; per-system hashes locate the system; `--threads 1` as the reference |
| Cascades are hard to reason about | Infinite loops or surprising order | Wave cap with a counter; the trace shows every wave; `next_tick` as the escape hatch |
| Checked spans cost in dev builds | Dev builds feel slow | Checks are per `ctx.read`/`write` call, not per element; nothing in ship |
| Broadphase becomes a physics engine | Scope creep | Grid, circles and AABBs, begin/end only; no response, no contacts |
| Wildcard resources hide a conflict | Two systems fight over a column | Wildcards expand to concrete ids at init; conflicts are reported on the concrete column |
| Schema macros get clever | Unreadable declarations | `KV_COMPONENT` lists fields by name; no reflection beyond offsets and scalar types |

**Milestones**

1. **Schemas and kinds**: component and kind tables, columns, typed spans and views, direct and command spawn, `SpriteWorld` migrated, `hello` unchanged.
2. **Schedule**: ids, in-phase DAG, rejection messages, checked access, per-system hashes, `--hash`.
3. **Events**: event schemas, handlers and waves, timer wheel, state triggers, stale-handle drop, trace ring.
4. **Broadphase and rocks**: `Collider2`, grid, overlap events, the game and its acceptance list, single-threaded.
5. **Jobs**: workers, chunk buffers, `parallel_for`, concurrent systems, hash equal across thread counts.
6. **Memory**: heap table and hard limits, attribution in the report, size-class allocator, dev guard pages.
