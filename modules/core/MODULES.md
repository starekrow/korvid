# Modules, and how code flows through a tick

The long version of two paragraphs in the README: what a module is and why the engine is
built out of them, how to write one, and what the engine does with each thing a module
declares when a tick runs. Read it once before writing a module; keep the README's quick
reference open while typing.

## What a module is

Modules are the top-level structure of the code. Everything above core is one: the
window and the input, the renderer, sprites, collision, assets, the frame loop, the CLI,
and the game itself. An executable is a list of modules in dependency order; a minimal
game links only the modules it names in `module.lua`, and the build generates the list.
Core is the host that runs what the list declares. When you ask where some behaviour
lives, the answer is a module; when you add behaviour, you add a module or extend one.

A module **owns the types the engine can see**. Component schemas, entity kinds and event
types are declared once, by the module they belong to, and from then on the engine knows
them by name: how to lay them out, hash them, compare them, print them, which of their
fields are handles. Other modules refer to them by that name and nothing else. The
sprite module declares `Transform2` and `Sprite`; a game kind that lists both is drawn;
the collision module declares `Collider2`; a kind that lists it is tested. No module
includes another's internals to make that happen, and the types are reflected, so tools,
the hash, the trace and the CLI read any module's data without that module's help.

A module **defines the coarse-grained flow of the tick**. The tick's phases are fixed
(Input, PreSim, Sim, PostSim); modules place systems in them and say what each reads and
writes. That is all the ordering a module ever states: the engine derives the fine order
within a phase from those sets, runs what is independent concurrently, delivers events
and applies spawns and destroys at the phase boundaries, and runs handlers there on the
batches that arrived. A module's view of time is therefore: my systems, in these phases,
over this data; my handlers, when these events come; my timers and triggers, when these
conditions hold. The rest is the engine's.

A module **owns its state and its memory**. Startup builds the module's state in an arena
the engine made for it; every system and handler of the module gets that state back;
anything the module allocates under its name, on any thread, is charged to it. Budgets,
leak reports and profiles are per module because the module is the unit of ownership.

A module is also the unit of **dependency** (`module.lua` deps, which also order startup),
of **documentation** (a README and documented headers, by the requirements), and of
**distribution** (modules are source; the ecosystem shares them as such).

What all of that comes down to in code is one `constinit ModuleDesc` and the functions
it points at. Nothing runs before `main`, nothing registers itself: `engine_init` reads
the generated list once and does everything in order, `engine_shutdown` undoes it in
reverse.

```cpp
struct ModuleDesc {
    const char*                name;
    Span<const EventTypeDesc>  events;     // plain-data event types
    Span<const ComponentDesc>  components; // schemas the engine lays columns out from
    Span<const KindDesc>       kinds;      // entity tables made of those components
    Span<const ResourceDesc>   resources;  // things systems read or write that are not columns
    Span<const SystemDesc>     systems;    // code in a phase, with its read and write sets
    Span<const HandlerDesc>    handlers;   // code at a phase boundary, on one event type
    Span<const TriggerDesc>    triggers;   // state edges watched for you
    Span<const CliCommandDesc> commands;   // what the CLI can be told
    bool (*startup)(Engine&, ModuleInstance&);
    void (*shutdown)(Engine&, ModuleInstance&);
    ModuleMemoryDesc           memory;     // the module arena's address space
};
```

Every table is optional. Order matters once: modules start in list order and stop in
reverse; ids are assigned in that order; systems that the schedule cannot otherwise
order keep module order then declaration order.

**State.** `startup` builds the module's state in the arena the engine made for it
(`arena_push<T>(self.arena)`) and records it with `self.set_state(s)`. Every system and
handler of the module gets it back as `ctx.state_as<T>()`, checked by type. The state is
an implicit resource named `<module>.state`, which every system of the module writes
unless its `SystemDesc` says `.state = StateUse::Read` or `None`; that is how two systems
of one module never race on it without anyone declaring anything.

**Ids.** `ComponentType<T>::id`, `EventType<E>::id`, the `id` slot of a `KindDesc` or
`TriggerDesc`, resource ids: all zero before `engine_init`, filled by it, zeroed by
shutdown. Look them up once through the context and cache them in a `static const u32`;
never keep one across two engines in one process.

## Writing one

### The files

```
modules/mygame/
  module.lua                      -- name, deps, output = "exe" for a game
  include/korvid/mygame/module.h  -- KV_MODULE_DECLARE(mygame_module)
  src/game.cc                     -- the tables and the code
  src/main.cc                     -- a FrameLoopConfig and one call to frameloop_main
  assets/                         -- copied to bin/assets/mygame/
  tests/                          -- one test executable, if any
```

```lua
module "mygame" {
  output = "exe",
  engine_module = true,
  deps = { "core", "platform", "math", "render", "asset", "sprite", "collision", "frameloop" },
}
```

### Components

A component is a plain struct with a field table. The table is the reflection
everything reads: the engine lays columns out from the size and alignment, hashes and
compares rows field by field (padding never counts), prints a row for `kv_handle_describe`
and the CLI, and knows which fields are handles.

```cpp
#include "korvid/math/fields.h"   // vec2, point2, rect2, color as fields

struct Velocity { vec2 v; f32 spin; };
struct Health   { i32 hp; };

constinit const FieldDesc k_velocity_fields[] = {KV_FIELDS(Velocity, v, spin)};
constinit const FieldDesc k_health_fields[]   = {KV_FIELDS(Health, hp)};
constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(Velocity, k_velocity_fields),
    KV_COMPONENT(Health, k_health_fields, CompPrev),   // keep last tick's copy: a trigger will watch it
};
```

Flags: `CompSim` (the default: hashed, snapshotted, visible to triggers), `CompCosmetic`
(presentation only; a sim system may not read it, checked at init), `CompPrev` (the
engine copies current into previous at the top of PreSim).

### Kinds

A kind is a named, fixed set of components and a capacity. There is no runtime add or
remove: every entity of the kind has every component. The engine makes it a table of
SoA columns on reserved pages (committed as rows are used), a handle kind named after
it, and resources `kind` (spawn and destroy) and `kind.Component` (each column).

```cpp
constinit u32 g_rock_kind = 0;
constinit const char* const k_rock_components[] = {"Transform2", "Sprite", "Velocity", "Collider2", "Health"};
constinit const KindDesc k_kinds[] = {
    {.name = "mygame.rock", .components = k_rock_components, .capacity = 1024, .id = &g_rock_kind},
};
```

Component names resolve across modules: `Transform2` and `Sprite` are the sprite
module's, `Collider2` the collision module's. A kind that has `Transform2` and `Sprite`
is drawn; one that has `Transform2` and `Collider2` takes part in the broadphase; neither
module needs to know your kind exists.

### Events

```cpp
struct RockHit      { Handle rock, bullet; };
struct ScoreChanged { i32 points, total; };

constinit const FieldDesc k_rock_hit_fields[] = {KV_FIELDS(RockHit, rock, bullet)};
constinit const FieldDesc k_score_fields[]    = {KV_FIELDS(ScoreChanged, points, total)};
constinit const EventTypeDesc k_events[] = {
    KV_EVENT_EX(RockHit, k_rock_hit_fields, 0),
    KV_EVENT_EX(ScoreChanged, k_score_fields, EvNextTick), // held until the next tick's first boundary
};
```

An event with a schema is checked at delivery: if a handle field no longer resolves,
the event is dropped and counted, so a handler never sees an entity that died at the
same boundary. `KV_EVENT(E)` without a schema is for events that carry no entity.

### Systems

A system is a function in a phase with the resources it reads and writes. The sets are
the whole contract: the scheduler orders systems by them, runs the independent ones
concurrently, and in dev builds asserts on any access outside them.

```cpp
void move(TickContext& ctx) {
    f32 dt = (f32)ctx.dt;
    for (KindRows k : ctx.view<Transform2, const Velocity>()) {   // every kind with both
        Span<Transform2>     t = k.col<Transform2>();
        Span<const Velocity> v = k.col<const Velocity>();
        parallel_for(ctx, k.rows(), 2048, [&](ChunkContext&, u32 b, u32 e) {
            for (u32 i = b; i < e; ++i) {
                if (!k.alive(i)) continue;
                t[i].position += v[i].v * dt;
                t[i].rotation += v[i].spin * dt;
            }
        });
    }
}

constinit const char* const k_move_reads[]  = {"*.Velocity"};
constinit const char* const k_move_writes[] = {"*.Transform2"};
constinit const char* const k_wrap_after[]  = {"mygame.move"};
constinit const SystemDesc k_systems[] = {
    {.name = "mygame.move", .phase = Phase::Sim, .fn = move, .reads = k_move_reads, .writes = k_move_writes},
    {.name = "mygame.wrap", .phase = Phase::Sim, .fn = wrap, .writes = k_move_writes, .after = k_wrap_after},
};
```

Naming resources: `mygame.rock` is the kind (a write on it covers spawn, destroy and
every column); `mygame.rock.Velocity` is one column; `*.Velocity` is that column in
every kind that has one; `input.state` and `platform.window` are published resources
reached with `ctx.resource<const Input>(...)`. `move` and `wrap` both write
`*.Transform2`, so one of them must say `after` the other or init fails with both names;
`move` reads what nobody in Sim writes, so it needs no edge of its own.

`.main_thread = true` pins a system to the thread driving the engine, for code that talks
to the OS, the GPU or a module's own pools (the asset pumps, the CLI). `.state = StateUse::None`
lets a system run beside its module's others. `.runs = RunWhen::Always` makes a system run
on paused ticks too (see below); the default, `Unpaused`, is right for gameplay.

### Cosmetic kinds

`KindDesc::cosmetic = true` keeps a kind out of the state hash altogether: glyphs, an
overlay, anything that exists only to be drawn and whose rows may differ between two
runs that are otherwise the same. The flow rule still applies per component, so a sim
system that reads such a kind's sim columns while writing sim state is a leak the
scheduler will not catch; do not do that.

### Pause

`engine_pause(e, true)` holds the simulation. The clock keeps producing ticks at its rate,
but they are *paused ticks*: the sim tick (`ctx.tick`, `Engine::clock.tick_count`) does
not advance, the timer wheel does not fire, triggers are not evaluated, and only systems
declared `RunWhen::Always` run. Handlers always run, since events can only come from
systems that ran. `ctx.paused` says which kind of tick a system is in. `engine_step(e, n)`
lets `n` sim ticks through a pause; `engine_rate(e, x)` scales time while unpaused. The
CLI has `pause`, `resume`, `step [N]` and `rate [X]`. The engine's own systems (input
sampling, the asset pumps, the sprite extract, the frame loop's exit, the CLI) are
`Always`, so a paused game still takes input, loads, draws and answers.

### Handlers

A handler runs at a phase boundary on the batch of one event type delivered there, under
the same set rules as a system. Handles in a delivered event are live, so a handler that
holds the write on a column may index it directly.

```cpp
void on_hit(TickContext& ctx, Span<const RockHit> hits) {
    static const u32 k_hp = ctx.column_id("mygame.rock.Health");
    Game& g = ctx.state_as<Game>();
    Span<Health> hp = ctx.write<Health>(k_hp);
    for (const RockHit& h : hits) {
        ctx.destroy(h.bullet);                                  // a command: takes effect at the boundary
        if (--hp[h.rock.index()].hp > 0) continue;
        ctx.destroy(h.rock);
        ctx.emit(ScoreChanged{20, g.score.total += 20});         // delivered next tick (EvNextTick)
        ctx.spawn_later(g_debris_kind, transform2_at(...), ...);  // a command: exists after the boundary
    }
}

constinit const char* const k_hit_writes[] = {"mygame.rock.Health"};
constinit const HandlerDesc k_handlers[] = {
    {.name = "mygame.on_hit", KV_ON(RockHit, on_hit), .writes = k_hit_writes},
};
```

### Timers and triggers

```cpp
u32 ticket = ctx.schedule_in(Expire{bullet}, 72);   // delivered at the first boundary 72 ticks from now
ctx.cancel(ticket);                                 // false if it already fired

constinit u32 g_death = 0;
constinit const TriggerDesc k_triggers[] = {
    {.name = "mygame.death", .field = "mygame.ship.Health.hp", .edge = TriggerEdge::CrossedBelow, .value = 1.0, .id = &g_death},
};
// A handler on StateEdge checks e.trigger == g_death. Requires CompPrev on Health.
```

### Startup, shutdown, the record

```cpp
struct Game { Score score; Handle rock_look; Rng rng; };

bool mygame_startup(Engine& engine, ModuleInstance& self) {
    Game* g = arena_push<Game>(self.arena);
    if (!g) return false;
    self.set_state(g);
    if (!engine_publish(engine, self, "mygame.score", &g->score)) return false; // others read it as a resource
    g->rock_look = asset_load_material(engine, "mygame/rock.material.json");
    g->rng = Rng::seeded(0x5eed);
    return true;
}

void mygame_shutdown(Engine&, ModuleInstance& self) {
    Game* g = self.state_as<Game>();
    KV_INFO("mygame", "final score %d", g->score.total);   // arrays in self.general: free them here
}

constinit const ResourceDesc k_resources[] = {{"mygame.score"}};

KV_MODULE_DEFINE(mygame_module) = {
    .name       = "mygame",
    .events     = k_events,
    .components = k_components,
    .kinds      = k_kinds,
    .resources  = k_resources,
    .systems    = k_systems,
    .handlers   = k_handlers,
    .triggers   = k_triggers,
    .startup    = mygame_startup,
    .shutdown   = mygame_shutdown,
    .memory     = {.reserve = 64 * 1024},
};
```

Startup runs in list order, so everything a module depends on is up: the renderer for
materials, the platform for its queue. A startup that returns false stops `engine_init`,
which shuts down what already started and returns false.

### Memory in a module

The module arena (`self.arena`) for what lives as long as the module; `self.general`, the
general allocator attributed to the module, for anything growable (set an `Array`'s or
`Map`'s `alloc` to `&self.general`, free in shutdown); `mem_scratch()` with a `MemScope`
for temporaries inside a call; kind tables for entities, which the module never
allocates. Whatever runs under a module's name (its startup, systems, handlers, jobs,
idle tasks) is charged to it, so `mem_report` and the leak report at exit name the
module, and a heap limit the root set for it applies.

## How code flows through a tick

The frame loop calls, in this order, every frame: `platform_pump` (OS events, raw input,
the I/O queue's pump), `engine_frame(e, dt)` (the ticks the clock owes, zero or more,
capped), idle work in the frame's slack, then the render. What follows is one tick.

```
tick N
  boundary (tick start)   timers due at N fire into pending
                          deliver: every writer's pending -> delivered, next-tick types included
                          handler waves, up to wave_cap:
                              run each handler that has new events, in unit order
                              apply commands
                              if nothing new is pending, stop; else deliver again (same-tick types only)
  phase Input             systems, level by level
                          apply commands
  boundary                deliver, handler waves, apply commands
  prev copy               every CompPrev column: current -> previous
  phase PreSim            systems, level by level; apply commands
  boundary                deliver, handler waves, apply commands
  phase Sim               systems, level by level; apply commands
  boundary                deliver, handler waves, apply commands
  phase PostSim           systems, level by level; apply commands
  triggers                each TriggerDesc compares previous and current rows; edges become pending StateEdge
  end of tick             delivered buffers are dropped; pending stays for tick N+1's first boundary
  hash                    if --hash: every sim column of every live row
```

**Levels.** Within a phase the scheduler has turned the systems into a DAG: a reader
after every writer of what it reads, `after` and `before` as drawn, module order then
declaration order for ties. Systems with no path between them share a level. A level's
systems run at once on the job pool (a pinned one runs on the main thread first), and
the next level waits for all of them. The schedule log at init prints every edge and
why, and the order per phase with `|` between levels.

**Events.** `ctx.emit(e)` appends to the running unit's writer; nothing is visible
until the next boundary, where every writer's pending events are merged in unit order
into the delivered batch for the type. `ctx.events->read<E>()` in a system returns the
whole batch delivered so far this tick; a handler receives only what the last deliver
added, so nothing is handled twice. A `next_tick` type waits for the next tick's first
boundary however early it was emitted. An event emitted by a handler in wave 0 is
delivered to the handlers of wave 1, and so on until the cap; what remains pending at
the cap rolls into the next boundary, with `waves_capped` counting.

**Commands.** `ctx.spawn_later`, `ctx.destroy` and `ctx.destroy_all` go into the running
unit's command buffer (one per parallel chunk, merged in chunk order). They are applied
after every phase and after every handler wave, in unit order: the engine's own buffer
first, then each unit's. A destroyed entity is still there for the rest of its phase;
readers in the same phase see it, events naming it afterwards are dropped. A command
spawn has no handle until it is applied; if `spawn_later` was asked for a ticket, a
`Spawned{ticket, kind, entity}` event carries the handle at the next delivery. A system
that holds the write on a kind spawns directly instead (`ctx.write_kind(k)->spawn(...)`)
and has the handle at once.

**Previous and current.** A `CompPrev` column is copied at the top of PreSim, so a Sim
system that moves things sees `prev<T>()` as where they were when the tick began. The
extract (sprite module, PostSim) compares the two to send only what moved to the
renderer; triggers compare them to find edges.

**Timers.** A scheduled event waits in the wheel keyed by tick and fires at that tick's
first boundary, in ticket order. A tick at or before the current one means the next.

**Parallel chunks.** Inside `parallel_for`, a chunk may emit and issue commands through
its `ChunkContext`; it may not schedule timers or spawn with a ticket. Chunk buffers are
merged in chunk order after the loop, so the result does not depend on which thread ran
which chunk. A worker's bare `mem_alloc` is charged to the module.

**Between ticks.** `engine_frame` returns; the frame loop spends the frame's slack on
idle tasks (`engine_idle_run`), then renders the world as of the last tick interpolated
by the clock's alpha. Several ticks in one frame run back to back, each a full tick.

## What the rules buy

- **The same world, the same hash.** Slot order, unit order, chunk order and ticket
  order are all fixed, so `--threads 1` and `--threads 16` agree, and two runs with the
  same seed agree. `--hash` prints the state hash per tick; `--hash-systems` after every
  system, which locates the system that introduced a difference.
- **No data races by construction.** Two systems that could touch the same column at
  once are never on the same level; the only shared mutable state inside a level is the
  general allocator, which is thread-safe.
- **Every access is a declaration.** The sets are documentation the scheduler enforces:
  the schedule log says what runs after what and why, and a system cannot quietly read
  something it did not say it would.

## Common mistakes

- **Two writers, no edge.** `engine_init` fails naming both systems and the resource.
  Say `after` on one of them.
- **A cycle.** A reads what B writes and B reads what A writes in one phase. The
  message prints the loop. Split one of them across phases, or let one read the previous
  tick's copy (`CompPrev`) instead.
- **Keeping a handle to something that may die.** Hold handles on events and in your
  own state, never in sim columns a trigger or a hash reads, and check `valid` or expect
  null from `get`. Delivered events have already been checked.
- **Reading a resource outside the sets.** Dev builds assert with the system and the
  resource named; ship builds trust you. Declare it.
- **An id across engines.** A tool that inits two engines in a row must look ids up
  again; the `static const u32` idiom is for one engine per process.
- **Work in startup that needs the tick.** Startup runs before any tick: no context, no
  events delivered, no commands applied. Spawn in the first tick (a state machine's INIT
  state, as the testbed does) or directly through `engine_table(e, kind)->spawn(...)`.
