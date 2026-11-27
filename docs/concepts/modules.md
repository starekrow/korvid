# Modules, and how code flows through a tick

What a module is and why the engine is built out of them, how to write one, and what the
engine does with each thing a module declares when a tick runs. Read it once before
writing a module; keep [core's quick reference](../../modules/core/README.md#quick-reference)
open while typing. [Scheduling](scheduling.md) and [cycles](cycles.md) are the short
versions of the ordering rules and of when each phase runs.

**Principles:** [2. Declarative stomps immediate](../principles.md#2-declarative-stomps-immediate), [3. Your declarations are a contract](../principles.md#3-your-declarations-are-a-contract), [4. Every byte has an owner](../principles.md#4-every-byte-has-an-owner), [6. Plain data at every boundary](../principles.md#6-plain-data-at-every-boundary)

## What a module is

Modules are the top-level structure of the code. Everything above core is one: the
window and the input, the renderer, sprites, collision, assets, the frame loop, the CLI,
and the game itself. An executable is a list of modules in dependency order; a minimal
game links only the modules it names in `module.lua`, and the build generates the list.
Core is the host that runs what the list declares. When you ask where some behaviour
lives, the answer is a module; when you add behaviour, you add a module or extend one.

A module **owns the types the engine can see**. Component schemas, ktables and event
types are declared once, by the module they belong to, and from then on the engine knows
them by name: how to lay them out, hash them, compare them, print them, which of their
fields are handles. Other modules refer to them by that name and nothing else. The
sprite module declares `Transform2` and `Sprite`; a game ktable that lists both is drawn;
the collision module declares `Collider2`; a ktable that lists it is tested. No module
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
    Span<const KTableDesc>     ktables;    // entity tables made of those components
    Span<const ResourceDesc>   resources;  // things systems read or write that are not columns
    Span<const SystemDesc>     systems;    // code in a phase, with its read and write sets
    Span<const HandlerDesc>    handlers;   // systems on one event type, handed its batch
    Span<const TriggerDesc>    triggers;   // state edges watched for you
    Span<const CliCommandDesc> commands;   // what the CLI can be told
    Span<const ModulePart* const> parts;   // more of the tables above (and startups), in other files
    bool (*startup)(Engine&, ModuleInstance&);
    void (*shutdown)(Engine&, ModuleInstance&);
    ModuleMemoryDesc           memory;     // the module arena's address space
};
```

Every table is optional. Order matters once: modules start in list order and stop in
reverse; ids are assigned in that order, except that ktables go by full name; systems
that the schedule cannot otherwise order keep module order then declaration order.

**State.** `startup` builds the module's state in the arena the engine made for it
(`arena_push<T>(self.arena)`) and records it with `self.set_state(s)`. Every system and
handler of the module gets it back as `ctx.state_as<T>()`, checked by type. The state is
an implicit resource named `<module>.state`, which every system of the module writes
unless its `SystemDesc` says `.state = StateUse::Read` or `None`; that is how two systems
of one module never race on it without anyone declaring anything.

**Ids.** `ComponentType<T>::id`, `EventType<E>::id`, the `id` slot of a `KTableDesc` or
`TriggerDesc`, resource ids: all zero before `engine_init`, filled by it, zeroed by
shutdown. Look them up once through the context and cache them in a `static const u32`;
never keep one across two engines in one process.

## Writing one

### The files

A game lives in a directory of its own (`examples/<name>/` in this repository, or anywhere
with a `korvid.config` above it naming the engine root: [kormake's manual](../../tools/kormake/README.md)):

```
mygame/
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

### KTables

A ktable is a named, fixed set of components and a capacity. There is no runtime add or
remove: every entity of the ktable has every component. The engine lays it out as SoA
columns on reserved pages (committed as rows are used), and makes a handle kind named
after it and resources `ktable` (spawn and destroy) and `ktable.Component` (each column).

```cpp
constinit u32 g_rock_ktable = 0;
constinit const KTableDesc k_ktables[] = {
    {.name = ".rock", .components = "Transform2, Sprite, Velocity, Collider2, Health", .capacity = 1024, .id = &g_rock_ktable},
};
```

Component names resolve across modules: `Transform2` and `Sprite` are the sprite
module's, `Collider2` the collision module's. A ktable that has `Transform2` and `Sprite`
is drawn; one that has `Transform2` and `Collider2` takes part in the broadphase; neither
module needs to know your ktable exists.

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
concurrently, and in dev builds asserts on any access through the context that they don't
cover. What a system reaches around the context (`ctx.engine`, another module's
functions) is its own business: keep that to main-thread systems, what never changes,
and functions that say they are safe from a system.

```cpp
void move(TickContext& ctx) {
    f32 dt = (f32)ctx.dt;
    for (KTableRows k : ctx.view<Transform2, const Velocity>()) {   // every ktable with both
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

constinit const SystemDesc k_systems[] = {
    {.name = ".move", .phase = Phase::Sim, .fn = move, .reads = "*.Velocity", .writes = "*.Transform2"},
    {.name = ".wrap", .phase = Phase::Sim, .fn = wrap, .writes = "*.Transform2", .after = ".move"},
};
```

**The name language.** Every name in a registration table is a string the engine reads
at init. A name that starts with `.` is the module's own: in module `mygame`, `.rock` is
`mygame.rock`, `.move` is `mygame.move`, `.rock.Velocity` is that ktable's column. A set
(`reads`, `writes`, `after`, `before`, a ktable's `components`) is one string of
comma-separated names; spaces around the commas are ignored. `?sprite_ui.layout` is an order
that holds only if that system exists (`?` then the name): its module may be missing from
the build, but if the module is there the system has to be, so a typo fails init. The same shorthand works at
runtime where the engine knows the module: `ctx.resource_id(".score")`,
`ctx.write_ktable(".rock")`, `engine_publish(engine, self, ".score", ...)`. Names of other
modules' things are written in full (`input.state`, `scene.compose`), as are names the
engine does not expand: an event type, a component, a handle kind, a clip's label.

Naming resources: `.rock` is the ktable (a write on it covers spawn, destroy, every column,
its attachments and its bags); `.rock.Velocity` is one column; `*.Velocity` is that column
in every ktable that has one; `*[Collider2].Transform2` is it in every ktable that also has
`Collider2`, `*[Collider2-Parent].Transform2` in those without `Parent`; `.npc.@Quest` and
`.npc.@props` are a ktable's attachments and bags; `input.state` and `platform_hid.window`
are published resources reached with `ctx.resource<const Input>(...)`. `move` and `wrap`
both write `*.Transform2`, so one of them must say `after` the other or init fails with
both names; `move` reads what nobody in Sim writes, so it needs no edge of its own. An
unknown name fails init with the system, the module and the name, as before.

Each name has one mode for a system: read (after every writer in the phase), read early
(`^`, the value the phase started with), write, or append (`appends`, see below). A
system whose names cover one resource two ways is refused, the whole ktable and one of its
columns included: one that wants the villagers as they stand but their positions as the
phase found them names the parts, `.npc.@alive, .npc.Actor, ^.npc.Transform2`
(`ktable.@alive` is the ktable's liveness: `ctx.alive(h)` and `ctx.rows(ktable)` read it). A
system cannot read early what it writes itself, and one that reads early from a writer and also reads, as it stands, something
that writer writes is refused unless the ktable is a snapshot (below). The whole model is
in [scheduling](scheduling.md); the design record is [docs/design/scheduler.md](../design/scheduler.md).

**Early reads, per ktable.** `KTableDesc::early_reads` says how a `^` read of the ktable gets
the phase's starting value. `EarlyReads::Order`, the default and nearly free, runs the
reader before the writers. `EarlyReads::Snapshot` copies what is read early and written
at the phase start and hands the reader the copy: the reader runs beside the writers, or
after them, and a cycle the order would make is not there; the copy costs every phase it
is needed (the `snapshot` zone, the `snapshot KB` counter). The game is the same either
way: `--early-reads order|snapshot|ktable` forces one for every ktable (`ktable`, the
default, leaves each its own), and the hashes must not move. The startup log shows each phase's levels as declared, all ordered and all
snapshots. A snapshot reader must reach the ktable through the context (`ctx.view`,
`ctx.read`, `ctx.rows`, `ctx.read_ktable`): `engine_ktable` hands out the ktable as it is now,
and dev builds refuse it there. Run with `--early-reads snapshot` in a dev build to find
any such read.

`.main_thread = true` pins a system to the thread driving the engine, for code that talks
to the OS, the GPU or a module's own pools (the asset pumps, the CLI). `.state` is what a
system does with its module's state: `Write`, the default, is `ctx.state_as<Game>()`;
`Read` is `ctx.state_as<const Game>()` only, beside the module's other readers; `None`
lets it run beside all of them and reach none of it (dev builds check). `.runs = RunWhen::Always` makes a system run
on paused ticks too (see below); the default, `Unpaused`, is right for gameplay.

### Mounts

A module that has things worth looking at mounts them in the CLI's filesystem: a
`CliMountDesc` in `ModuleDesc::mounts` with a `path` and three functions, `list` (the
children of one directory), `describe` (one node) and `patch` (optional: set one). The
asset module mounts `/materials`, `/textures`, `/fonts` and `/sounds`; the engine's own
`/ktables`, `/systems`, `/clock` and the rest come with the cli module. See
[docs/design/cli.md](../design/cli.md). A command is for an action; a mount is for state.

### Append resources

`ResourceDesc{".commands", audio_apply}` declares an append resource: something any
number of units hand records to, which its owner applies. A system declares
`.appends = "audio.commands"` and calls `ctx.append(resource, record)` (audio wraps it:
`audio_play` and the rest); the record goes into the unit's command buffer, and at the
phase boundary, with the spawns and destroys, the engine hands each to `audio_apply`, in
unit order, whatever thread ran what. Appends need no order between units or with
anything else. Only the owner's systems write an append resource; anyone else declaring a
write on it is told to append.

Commands (`spawn_later`, `destroy`, `destroy_all`, `name_later`) need no declaration at
all: they land at the boundary when nothing runs. Spawning or destroying at once takes
`write_ktable`, which needs the write on the ktable.

### Cosmetic ktables

`KTableDesc::cosmetic = true` keeps a ktable out of the state hash altogether: glyphs, an
overlay, anything that exists only to be drawn and whose rows may differ between two
runs that are otherwise the same. The flow rule still applies per component, so a sim
system that reads such a ktable's sim columns while writing sim state is a leak the
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

A handler is a system on one event type: it has a phase, runs in the order the schedule
gives it like any system (after every emitter of its event in the phase), and is handed
its batch, the events emitted since it last ran; with none it is not called (paused ticks
included, so a handler sees what Always systems and the engine emit). It declares what it
emits in `emits` like any system. Handles in its batch are live, so a handler that holds
the write on a column may index it directly. Where to put one that used to run at a
boundary: at the end of the phase that emits its event when its writes are the next
phase's start (arcanum's level build), or at the start of the next phase, reading early
(`^`) what that phase writes, when systems earlier in the emitting phase read what it
writes (arcfire's hits, in PostSim). The scheduler refuses the wrong one by name.
[docs/design/events.md](../design/events.md) has the model.

```cpp
void on_hit(TickContext& ctx, Span<const RockHit> hits) {
    static const u32 k_hp = ctx.column_id(".rock.Health");
    Game& g = ctx.state_as<Game>();
    Span<Health> hp = ctx.write<Health>(k_hp);
    for (const RockHit& h : hits) {
        ctx.destroy(h.bullet);                                  // a command: takes effect at the boundary
        if (--hp[h.rock.index()].hp > 0) continue;
        ctx.destroy(h.rock);
        ctx.emit(ScoreChanged{20, g.score.total += 20});         // delivered next tick (EvNextTick)
        ctx.spawn_later(g_debris_ktable, transform2_at(...), ...);  // a command: exists after the boundary
    }
}

constinit const HandlerDesc k_handlers[] = {
    {.name = ".on_hit", KV_ON(RockHit, on_hit), .writes = ".rock.Health"},
};
```

### Timers and triggers

```cpp
u32 ticket = ctx.schedule_in(Expire{bullet}, 72);   // delivered at the first boundary 72 ticks from now
ctx.cancel(ticket);                                 // false if it already fired

constinit u32 g_death = 0;
constinit const TriggerDesc k_triggers[] = {
    {.name = ".death", .field = ".ship.Health.hp", .edge = TriggerEdge::CrossedBelow, .value = 1.0, .id = &g_death},
};
// A handler on StateEdge checks e.trigger == g_death. Requires CompPrev on Health.
```

### Startup, shutdown, the record

```cpp
struct Game { Score score; Handle rock_look; };

bool mygame_startup(Engine& engine, ModuleInstance& self) {
    Game* g = arena_push<Game>(self.arena);
    if (!g) return false;
    self.set_state(g);
    if (!engine_publish(engine, self, ".score", &g->score)) return false; // others read it as a resource
    g->rock_look = asset_load_material(engine, "mygame/rock.material.json");
    return true;
}

void mygame_shutdown(Engine&, ModuleInstance& self) {
    Game* g = self.state_as<Game>();
    KV_INFO("mygame", "final score %d", g->score.total);   // arrays in self.general: free them here
}

constinit const ResourceDesc k_resources[] = {{".score"}};

KV_MODULE_DEFINE(mygame_module) = {
    .name       = "mygame",
    .events     = k_events,
    .components = k_components,
    .ktables    = k_ktables,
    .resources  = k_resources,
    .systems    = k_systems,
    .handlers   = k_handlers,
    .triggers   = k_triggers,
    .startup    = mygame_startup,
    .shutdown   = mygame_shutdown,
    .memory     = {.reserve = 64 * 1024},
};
```

A module needs no `Rng` of its own for the sim: `ctx.rng()` is the module's stream,
seeded by the engine from the module's name and `--seed`, and part of the state hash.

Startup runs in list order, so everything a module depends on is up: the renderer for
materials, the platform for its queue. A startup that returns false stops `engine_init`,
which shuts down what already started and returns false.

### Parts: a module of many files

One record gets unwieldy once a game has a few dozen systems: the ship's component, its
ktables and its systems sit in the record's file, far from the ship's code, and every
system function needs a prototype for the record to name it; and the startup loads every
file's art. A `ModulePart` holds the same tables as the record (everything but the name and
memory) and a startup and shutdown of its own, so each file declares what it brings at its
end, next to the code, loads what it needs, and the record lists the parts:

```cpp
// ship.cc, after the ship's systems
namespace ship_part {
constinit const FieldDesc     k_ship_fields[] = {KV_FIELDS(Ship, cooldown, lives)};
constinit const ComponentDesc k_components[]  = {KV_COMPONENT(Ship, k_ship_fields)};
constinit const KTableDesc    k_ktables[]     = {{.name = ".ship", .components = "Transform2, Sprite, Ship", .capacity = 1, .id = &g_ship_ktable}};
constinit const SystemDesc    k_systems[]     = {{.name = ".control", .phase = Phase::Sim, .fn = control, .writes = ".ship"}};
bool startup(Engine& e, ModuleInstance& self) { // after mygame_startup: its Game is there
    self.state_as<Game>()->ship_tex = asset_load_texture(e, "mygame/ship.png");
    return true;
}
}
KV_MODULE_PART(mygame_ship) = {.components = ship_part::k_components, .ktables = ship_part::k_ktables, .systems = ship_part::k_systems,
                               .startup = ship_part::startup};

// game.h:  KV_MODULE_PART_DECLARE(mygame_ship);  and the others
// game.cc
constinit const ModulePart* const k_parts[] = {&mygame_ship, &mygame_enemies, &mygame_hud};
KV_MODULE_DEFINE(mygame_module) = {.name = "mygame", .components = k_shared_components, .parts = k_parts, .startup = mygame_startup};
```

`engine_init` joins them into one record, the module's own tables first and then each
part's in list order, and everything after works on that: names are the module's (`.ship`
is `mygame.ship` from any part), a part may use what another declared, a name declared in
two parts is refused like one declared twice in the record. Each file's tables need names
of their own (a namespace per file, as above): a unity build compiles every file of the
module as one.

A part's startup runs after the module's, the parts in list order, and gets the module's
instance: the state the module's startup made is there for it to fill, and what it takes
from `self.general` or the arena is the module's. One that returns false fails
`engine_init` like the module's would, and what started before it is shut down. Shutdown is
the reverse: the parts' shutdowns from the last part to the first, then the module's.

The list's order is the declaration order the schedule falls back on, so it is worth
writing in the order the game runs. Systems that write the module's state (the default)
conflict on it, and two in one phase with nothing else between them run in that order.
A ktable's place in the list does not matter: ktables get their ids by full name, so a
view walks them in the same order wherever they are declared. arcfire is built this way:
`module.cc` keeps Velocity (which every file moves by), the world ktable and the startup
that makes the game's state, and each of its other files ends with its part: its
components, its ktables, its code, and a startup that loads its textures and files.

### Memory in a module

The module arena (`self.arena`) for what lives as long as the module; `self.general`, the
general allocator attributed to the module, for anything growable (set an `Array`'s
`alloc` to `&self.general`, or call a `Map`'s `init(&self.general)`; free in shutdown);
`mem_scratch()` with a `MemScope`
for temporaries inside a call; ktables for entities, which the module never
allocates. Whatever runs under a module's name (its startup, systems, handlers, jobs)
is charged to it, so `mem_report` and the leak report at exit name the
module, and a heap limit the root set for it applies.

## How code flows through a tick

The frame loop calls, in this order, every frame: `platform_pump` (OS events, raw input,
the I/O queue's pump), `engine_frame(e, dt)` (the ticks the clock owes, zero or more,
capped), idle work in the frame's slack, then the render. What follows is one tick.

```
tick N
  tick start              timers due at N fire; held next-tick events and the engine's writer merge into the streams
  phase Input             units (systems and handlers) level by level, each level's consumed streams merged first;
                          then the phase's events, its commands, and the engine's writer (the commands' Spawned)
  prev copy               every CompPrev column: current -> previous (a large ktable's on the job pool)
  phase PreSim            the same
  phase Sim               the same
  phase PostSim           the same
  triggers                each TriggerDesc compares previous and current rows; edges go to the engine's writer
  end of tick             each stream keeps what a consumer has not reached
  hash                    if --hash: every sim column of every live row
```

**Levels.** Within a phase the scheduler has turned the systems into a DAG: a reader
after every writer of what it reads, `after` and `before` as drawn, module order then
declaration order for ties. Systems with no path between them share a level. A level's
systems run at once on the job pool (a pinned one runs on the main thread first), and
the next level waits for all of them. The schedule log at init prints every edge and
why, and the order per phase with `|` between levels.

**Events.** An event type is a stream. `ctx.emit(e)` appends to the running unit's
writer, declared in `emits`; a unit that names the type in `reads` runs after every
emitter of it in its phase and takes `ctx.received<E>()`: what was emitted since it last
ran, in unit order however the levels fell, every event once. With `^` the batch ends at
the phase start and needs no order, which is how a loop goes round (once a phase). A
`next_tick` type waits for the next tick's start. The engine's writer (`e.events.emit`)
is for what happens outside the schedule; its events land at the next boundary. An event
naming a handle that no longer resolves is dropped from each batch it would be in. Code
outside the schedule reads the tick's events with `e.events.read<E>()`; from inside a
unit that is refused (dev builds). The order is a type's own: two types are two
streams, and nothing records which of their events came first. Facts whose order matters
go in one type with a field that says which, or into a resource the events only announce.

**Commands.** `ctx.spawn_later`, `ctx.destroy` and `ctx.destroy_all` go into the running
unit's command buffer (one per parallel chunk, merged in chunk order). They are applied
after every phase, in unit order: the engine's own buffer first, then each unit's. A
destroyed entity is still there for the rest of its phase; readers and consumers in the
same phase see it, batches after it drop events naming it. A command
spawn has no handle until it is applied; if `spawn_later` was asked for a ticket, a
`Spawned{ticket, ktable, entity}` event carries the handle, from the boundary after the phase. A system
that holds the write on a ktable spawns directly instead (`ctx.write_ktable(k)->spawn(...)`)
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
the Work cycle (`engine_work`), runs the Frame cycle (`engine_frame_cycle`), then renders
the world as of the last tick interpolated by the clock's alpha. Several ticks in one
frame run back to back, each a full tick. [Cycles](cycles.md).

## What the rules buy

- **The same world, the same hash.** Slot order, unit order, chunk order and ticket
  order are all fixed, so `--threads 1` and `--threads 16` agree, and two runs with the
  same seed agree. `--hash` prints the state hash per tick; `--hash-systems` after every
  system, which locates the system that introduced a difference.
- **No data races by construction.** Two systems that could touch the same column at
  once are never on the same level; the only shared mutable state inside a level is the
  general allocator, which is thread-safe.
- **Every access is a declaration.** The sets are documentation the scheduler enforces:
  the schedule log says what runs after what and why, and a system cannot quietly read,
  through its context, something it did not say it would.

## Common mistakes

- **Two writers, no edge.** `engine_init` fails naming both systems and the resource.
  Say `after` on one of them.
- **A cycle.** A reads what B writes and B reads what A writes in one phase. The
  message prints the loop. Split one of them across phases, or let one read the previous
  tick's copy (`CompPrev`) instead.
- **Using a handle to something that may have died.** A handle anywhere (an event, your
  state, a sim column: the engine's own `Parent` and `MoveResult2` keep them there) may
  name an entity that has since gone; it is generational, so it stops resolving rather
  than pointing at whatever took the slot. Check `valid`, or expect null from `get`.
  Delivered events with a schema have already been checked.
- **Reading a resource outside the sets.** Dev builds assert with the system and the
  resource named; ship builds trust you. Declare it.
- **An id across engines.** A tool that inits two engines in a row must look ids up
  again; the `static const u32` idiom is for one engine per process.
- **Work in startup that needs the tick.** Startup runs before any tick: no context, no
  events delivered, no commands applied. Spawn in the first tick (a state machine's INIT
  state, as the testbed does) or directly through `engine_ktable(e, name)->spawn(...)`.
