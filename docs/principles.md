# Principles

> **Draft.** These are read out of the code, the design records and the commit history,
> not written by the project's author. They are here to be rewritten, reordered, merged
> or struck out. Until the author has been through them, treat them as a description of
> what the code does, not a statement of intent.

These are the ideas Korvid is built on: why the engine looks the way it does. Every other
document describes how something works. This one describes what the parts have in
common, so that someone extending the engine (or deciding where to bend it) can make the
same choices the existing code made, for the same reasons.

Each principle gives its reason, what it costs, where it shows up, and how the engine
holds code to it. A principle with no cost isn't a choice. The costs, and the
[tie-breaks](#when-principles-pull-apart) at the end, are what let you apply these to a
case they don't mention.

## 1. Same input, same game

A run is a function of its starting state and its input. The same seed and the same
input give the same state, tick for tick, on every platform, build and thread count.

**Why.** A bug you can replay is a bug you can bisect, down to one tick and one system. A
change that shouldn't affect the simulation can be shown not to. Netplay, saves and
recorded demos all rest on it.

**What it costs.** No platform `libm` in the sim, no `rand()`, no clock reads, no
iteration over anything in address or thread order. The scheduler numbers units by
declaration, not by when they ran, and merges parallel work in a fixed order, which takes
some freedom away from the job pool. Every floating-point build flag that could fuse or
reorder operations is off.

**Where it shows.** The sim's fixed tick ([cycles](concepts/cycles.md)). Unit numbering
and chunk-order merges ([scheduling](concepts/scheduling.md#results-dont-depend-on-the-schedule)).
Seeded `Rng` streams in module state (`korvid/core/random.h`). The math module's own
transcendentals ([math](../modules/math/README.md)). No fused or reordered floating point
in any toolchain (`-ffp-contract=off`; `/fp:precise` with cl). A fixed, never-seeded hash (`korvid/core/hash.h`). Map iteration in insertion
order (`korvid/core/map.h`).

**How it is held.** The state hash: `--hash` every tick, `--hash-systems` every system.
Runs at `--threads 1` and `--threads 16` must agree. `--early-reads order` and
`snapshot` must agree. `--replay` compares the hashes in a recording as it plays. Frame
and Work units can't write the sim: init refuses the declaration, and the context
refuses the spawn ([cycles](concepts/cycles.md#the-rule-only-the-sim-writes-the-sim)).

## 2. Declare, don't do

A module says what it brings as data (its components, kinds, systems, events, resources,
commands) in one `constinit` record. The engine reads every record once, at
`engine_init`, and from then on knows each type by name.

**Why.** What is declared can be checked, ordered, printed, hashed, saved and shown in a
tool without the module's help. Nothing happens behind the reader's back: there is no
static initializer order to reason about, no registration that depends on link order,
and a module's whole surface is in one place.

**What it costs.** Tables to write: a field table per component, a row per system.
Nothing can be added at runtime: no new kinds or systems after init (runtime module
loading is designed for, not built). Some things that would be one line of code elsewhere are a declaration and a
lookup here.

**Where it shows.** `ModuleDesc` (`korvid/core/module.h`). Field tables as the one
reflection everything reads (`korvid/core/component.h`). The name language,
`.x` for the module's own ([Modules](concepts/modules.md#systems)). The generated
module list.

**How it is held.** `constinit` makes a dynamic initializer a compile error. An unknown
name in any table fails `engine_init` and names it. A component with an unknown field
type fails to compile.

## 3. The declaration is the contract

What a system says it reads and writes is the whole of what it may touch. The engine
derives the order from those sets and refuses at startup anything that would race or
contradict itself. The schedule is never written by hand.

**Why.** Ordering bugs (something happens a tick early, or late, or either depending on
the machine) are among the most expensive bugs a game has. An order solved from
declarations can be explained: the startup log says why each edge exists. Modules
compose without knowing each other: collision runs after a game's movers because both
name `Transform2`.

**What it costs.** Every access is declared, including ones that feel obvious. Some
designs are refused outright, and the fix is to restructure (split a phase, read early,
make a kind a snapshot) rather than to override. Dev builds pay for the checks.

**Where it shows.** `SystemDesc` and `TickContext` (`korvid/core/phase.h`). The resource
tree and the modes ([scheduling](concepts/scheduling.md)). Appends for writers that need
no order. Cosmetic components that can't steer the game.

**How it is held.** `engine_init` refuses two writers with no order, cycles and torn
reads, naming every unit and resource involved. In dev builds, an access through the
context that the sets don't cover stops the program and names it. A sim system that
reads a cosmetic component fails init.

## 4. Every byte has an owner

Every allocation is charged to someone: the module whose code was running, a category,
a heap with limits the executable chose. Lifetimes are explicit: a module's arena, a
frame's scratch, a kind's table.

**Why.** "Where did the memory go" has an answer, per module. A budget can be enforced
before it is blown, rather than discovered on a console. Leaks are found at the exit of
every dev run, not in a profiler session months later.

**What it costs.** Code threads allocators through, chooses a lifetime for what it
allocates, and frees growable containers in shutdown. There is no garbage collector, no
RAII container that cleans up after you, and no STL.

**Where it shows.** The allocator table in `korvid/core/memory.h`. `self.arena` and
`self.general` in every module. The scratch arena and `MemScope`
(`korvid/core/arena.h`). Heaps in `EngineConfig`.

**How it is held.** `mem_report` and the leak report at exit, naming module and
category. In debug and dev builds, a tool exits with status 3 when anything is still live after
shutdown. A heap's
hard limit fails the allocation and names it. Single-owner arenas and pools assert their
thread.

## 5. Fail loudly, with names

Failure is a return value (null, false, a null handle), and the place that detects it
logs what failed, naming everything involved and, where it can, the fix. A failure that
would repeat is logged once and counted.

**Why.** A message that names the unit, the resource and the fix is a minute's work. An
exception unwinding through a game loop, or a silent fallback that limps on, is an
afternoon's. Code that can't fail in the normal case shouldn't pay for exceptions.

**What it costs.** Callers check returns. Messages are written with care, and kept
accurate as the code changes. Nothing is caught and handled far from where it went
wrong.

**Where it shows.** `engine_init`'s refusals ("`rocks.move` and `rocks.wrap` both write
... add `after` or `before`"). A full kind's spawn, logged once and counted in
`Table::refused`. Asset failures as `AssetFailed` events with a placeholder in their
place. Asserts that print the expression and the line.

**How it is held.** Exceptions and RTTI are off in every toolchain (`-fno-exceptions
-fno-rtti`; `/EHs-c- /GR-` with cl). The habit of review:
a new failure path that returns silently is a bug.

## 6. Plain data at every boundary

What crosses between modules, between ticks, between threads, or into a file is plain
data: structs you can copy with `memcpy`, handles instead of pointers, field tables
instead of virtual functions.

**Why.** Plain data can be hashed, saved, sent, snapshotted, compared and printed by
code that knows nothing about it. A handle survives storage moving and tells you when
its target is gone, which a pointer can't.

**What it costs.** No constructors or destructors that matter, no inheritance at
boundaries, an extra lookup to turn a handle into a pointer. Behaviour lives in systems,
not in objects.

**Where it shows.** Components, events and resources are trivially copyable (asserted).
`Handle` (`korvid/core/handle.h`). `Span`, `Array` and `Map` over plain data.

**How it is held.** `static_assert`s on components, events, containers and appends:
"components are plain data".

## 7. Checks cost in dev, never in ship

A dev build checks what it can afford to: ownership, alignment, canaries, poison,
declared access. A ship build checks none of it and pays nothing for it.

**Why.** The checks are what make the other principles hold in practice, so developers
should run with them all day. Players shouldn't pay for them.

**What it costs.** Dev and ship builds time differently. A bug that only a check would
have caught surfaces in ship as plain misbehaviour, so the checks have to be exercised
in dev.

**Where it shows.** `KV_DEV` and `KV_DEBUG` (`korvid/core/types.h`). `KV_VERIFY` and
`KV_ASSERT`. `--spike-hunt` for a dev build timed close to ship.

**How it is held.** Measurement. The scheduler's checks were timed in both builds before
they were kept: ship paid nothing measurable ([docs/design/scheduler.md](design/scheduler.md)).

## 8. Measure before believing

A design decision that claims to be fast, or cheap, or free comes with the numbers.
Design records carry their measurements; commits say what was measured or found.

**Why.** Performance intuitions are wrong often enough to be expensive. A number in the
record also tells the next person when the trade-off has stopped holding.

**What it costs.** Time: benches, alternated runs, results tables, the commit that says
"0.96 ms against 1.09" instead of "faster".

**Where it shows.** The results sections of the [design records](design/). The commit
convention in [CONTRIBUTING.md](../CONTRIBUTING.md). cpukiller and arcfire's `--stress`
as standing benches.

**How it is held.** By review and by habit: a performance claim without a number is a
question, not a result.

## 9. Few dependencies, owned

The engine depends on a C++ compiler, the platform's graphics and windowing, and little
else. What it needs beyond that (inflate, a Vorbis decoder, TrueType, XML, Lua) is
vendored, pinned and compiled into the engine under its allocator and its math.

**Why.** Builds that take seconds, run anywhere the compiler does, and never break
because something upstream changed. Vendored code also obeys the other principles: its
allocations are owned, and its math is deterministic.

**What it costs.** Upgrading a vendored library is a deliberate job (`vendor/UPGRADE.md`
in the lua module). Some things are written here that a package would have provided.

**Where it shows.** `src/vendor/` in the [inflate](../modules/inflate/README.md),
[xml](../modules/xml/README.md) and [lua](../modules/lua/README.md) modules. The
[project README](../README.md#minimal-dependencies). kormake itself.

**How it is held.** Review: a new external dependency is a design decision with its own
record.

## 10. Say it once

One hash, one text encoding (UTF-8), one clock, one way to declare a type, one home for
each fact in the docs. When there are two ways to do something, one of them goes.

**Why.** Two ways to do one thing drift apart, and every reader has to learn both. The
scheduler's rewrite started from finding that "what does this declaration cover?" was
answered twice, differently.

**What it costs.** Unification work when two ways appear. Indirection: a document links
rather than repeats, and code calls the one function instead of writing its own.

**Where it shows.** `hash.h`, `utf8.h`, `tick.h`. The resource tree, expanded once and
used by both the schedule and the checks ([docs/design/scheduler.md](design/scheduler.md)).
"Each fact has one home" in [DOCSTYLE.md](../DOCSTYLE.md).

**How it is held.** [tools/doccheck.sh](../tools/doccheck.sh) for the docs. Review for
the code.

## When principles pull apart

They mostly agree, but where they meet, this is the order the code has chosen so far.
These choices are worth stating, and challenging:

- **Determinism over speed.** Units are numbered by declaration even where running them
  in another order would be faster. The scheduler record measures what that costs.
- **A refused design over a silent one.** The scheduler refuses a torn read rather than
  guessing which value was meant.
- **Ship's speed over dev's.** Checks may cost a dev build several percent; a ship build
  pays nothing.
- **An explicit lifetime over a convenient one.** Code chooses an arena or frees in
  shutdown, rather than reaching for something that cleans up by itself.
- **Fewer dependencies over less code.** A small decoder is vendored rather than linked
  from the system.

## Where these show up

Concept pages and header file comments say which principle they serve, where one is the
reason for the design ([DOCSTYLE.md](../DOCSTYLE.md#principles)). Design records say
which principle they defended or traded off. When a change seems to need breaking one of
these, that is a design record of its own, written before the change.
