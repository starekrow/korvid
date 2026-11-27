# cpukiller

A stress tool for the job pool, the memory system and the engine's tick rate. No window
by default. It measures what the pool costs to dispatch, how contention scales with chunk
size, whether nested batches make progress, how the allocator behaves under cross-thread
load, and how many ticks per second a few hundred thousand entities get on one thread
against many, with the state hash as the referee. Numbers go to stdout as a table;
every scenario checks that the memory accounting comes back to where it started.

## Building and running

From the repository root, once, to build the build tool:

```bash
tools/kormake/bootstrap.sh
```

Then build the configuration you want and run the executable from its bin directory
(no window, no assets: the tool runs anywhere):

```bash
tools/kormake/bin/linux/kormake cpukiller dev    # or, in this directory: kormake dev
build/linux-dev/bin/cpukiller --quick
```

`kormake cpukiller` builds this example and what it links, in linux-debug unless a config follows:
debug is everything checked with the validation layers on, `dev` the build to measure
with, `ship` neither. `kormake test cpukiller` runs its tests; at the root, `kormake` builds
every module and example and `kormake test` runs every suite.
The debug build runs too, but it measures its own checks: use dev for numbers.

## Reading the table

Every number is a median of repeats (`--reps N`; each scenario has its own default), with
the ways of running a case alternated, so a clock that moves or a core that warms moves
all of them. Three ways stand beside each other:

- **serial**: a plain loop on the calling thread, no pool.
- **pool**: the engine's job pool at `--threads`.
- **reference**: the least that could be done (`src/floor.h`). For the pool, threads that
  never sleep and claim chunks with one atomic add: no lock, queue, wake-up, zone or
  attribution, and every core burning while it is up, so a bound and not a design. For the
  allocator, the C runtime's `malloc` on the same threads. For the engine, the same four
  systems over bare arrays on one thread.

**spread** is the pool's slowest repeat less its fastest, as a share of its median: how far
to trust the row. Latency rows have their p99 and worst in the notes instead.

## The scenarios

1. **Dispatch overhead.** Batches of chunks that do nothing, from 2 to 65,536 chunks, each
   batch timed: what one batch costs to wake the pool and what one chunk costs at scale.
2. **Wake latency.** One chunk a thread of about 50 µs each, from a pool that is awake and
   from one that has gone to sleep: how small a batch is worth sending.
3. **Contention.** Fixed work cut into chunks from tiny to large: the grain below which
   handing out chunks is the bottleneck, if there is one, and the speedup it caps.
4. **Nested batches.** Every outer chunk runs an inner batch: the pool must not deadlock
   and the caller must help, which it does because a waiter takes chunks from other batches.
5. **Memory under load.** Same-thread allocate and free (small blocks barely touched, and
   mixed sizes filled, which tests the memory bus as much as the allocator), remote free
   (allocated on one thread, freed on another), scratch arenas and arrays.
6. **The engine.** The `cpukiller.body` ktable (`Pos` with `CompPrev`, `Vel`, `Age`) at
   `--bodies` entities, four systems (integrate in a `parallel_for`, bounce with an event
   per bounce, churn by chunk commands, a reduction) and a handler that eats the events,
   run for `--ticks` ticks at one thread and at `--threads`, alternated; every run's hash
   must be equal. Each tick is timed, for the tail. The breakdown comes from the
   profiler's collector, read every frame: the top zones of each run and the workers' time
   split into busy and waiting (dev builds; ship has no profiler).

`--sweep` runs the cases that scale (two grains of contention, two of the allocator, the
engine, and the bare arrays) at 1, 2, 4... threads up to `--threads`, and prints speedup
by thread count instead of the table.

With `--window` the usual stack comes up first (platform, renderer, frame loop), the
pool and memory scenarios run while the window and swapchain are alive, and the engine
ticks are driven through `frameloop_step` with a render every 16 ticks: memory can
behave differently with a GPU's fingers in it.

`tools/concurrency-numbers.sh` wraps a run and a sweep in the machine, the build and the
power setting they were taken with, as Markdown.

## What it found

On a Ryzen 7 7840U (8 cores, 16 threads), Windows 11, Balanced plan on mains, Nov 26,
2026, ship build unless said. "Before" is the first pool: every chunk queued by itself,
the pool's lock taken twice a chunk, a broadcast to every sleeper after each, no spin, and
the previous copy on the main thread.

| | before | now | reference |
|---|---|---|---|
| chunks of 0.5 µs, 16 threads | 1.9x | 13.4x | 12.7x |
| chunks of 4 µs and up | 13.6x to 14.3x | 13.3x to 14.4x | 13.4x to 14.1x |
| a chunk's dispatch, at scale | 181 ns | 15 ns | 14 ns |
| nested, 64 batches of 64 | 1.3x | 8.4x | |
| 16 chunks of 52 µs, pool awake | 139 µs | 87 to 106 µs | 57 µs (ideal 52) |
| the same, pool asleep | 230 µs | 216 to 226 µs | |
| 2 empty chunks, a batch | 0.2 µs | 3.8 µs | 0.6 µs |
| 16 empty chunks, a batch | 6.5 µs | 8.0 µs | 1.1 µs |
| an engine tick, 16 threads | 397 to 444 µs (2.4x to 2.7x) | 196 µs (5.5x) | bare arrays 119 µs (6.5x) |
| engine ticks by threads: 2, 4, 8, 16 | 1.6x, 2.5x, 3.1x, 2.8x | 1.8x, 3.3x, 5.2x, 5.3x | 1.9x, 2.9x, 5.5x, 6.5x |

- **The pool is at the floor at every grain.** Taking a chunk is one atomic add; the lock
  is taken once for each batch a thread joins.
- **The engine tick came down in three steps.** The claim alone: nothing (444 µs). The
  idle spin: 295 µs. The previous copy on the pool (`KTable::begin_tick`): 196 µs. The
  copy was one serial `memcpy` of about 90 µs, about as long as the spin, so the workers
  were asleep again when the first system's batch arrived; as a batch it is shorter and
  keeps them there.
- **The spin's length.** Before the copy moved, `--only engine --spin N`: 444 µs a tick
  with none, 295 with 100 µs, 227 to 268 with 500, and a p99 of about 600 µs up to 100
  and 800 to 1,150 at 500. The default is 100.
- **Chunk size is not the limit.** Chunks half and a quarter the size: no change. An
  eighth and a sixteenth: slower by 40 to 110 µs a tick, on one thread too, since every
  chunk has its own event writer and command buffer, made and merged on the caller.
- **What is left between the engine and bare arrays** (196 µs against 119): a chunk's
  work is 1.3 to 1.4 times the bare loop's (events, commands, liveness), and about 70 µs
  a tick (dev, traced) goes to waiting for the last chunks and merging the buffers. The
  hardware bounds both: a chunk that takes 4.3 µs alone takes 8 to 11 with sixteen
  threads at it, for bare arrays the same, where pure computation scales 14x.
- **A batch of nothing costs more than it did.** Workers that are awake join a batch at
  once, and the caller waits for them to leave: 3.8 µs for two empty chunks where the
  first pool took 0.2 because nobody had woken yet. A batch with work in it is what the
  pool is for; one without is the case to run on the caller.
- **Waking a sleeping pool is the OS's price**, and unchanged: about 170 µs over ideal.
- **A loaded machine stalls a 16-thread tick.** With another program on 12 cores, ticks
  of 60 to 360 ms appeared; on a quiet machine the worst of 5,400 was 1.1 ms. A batch
  waits for every thread that joined it, and a thread the OS has taken away is one.
- **The allocator beats `malloc` in ship.** Small blocks 353 M/s against 271, filled
  blocks 181 against 40, remote frees 57 against 3.3.
- **The allocator does not scale in dev.** Small blocks on 16 threads take as long as on
  one (25 M/s against `malloc`'s 285): the dev build's bookkeeping is shared between the
  threads.
- **The workers wait 32% of their time** in the engine scenario (dev), from 41%.
- **Every hash equal**, at every thread count, in every run.

## Where things are

| file | what |
|---|---|
| `src/main.cc` | the scenarios, the report, the window mode, `main` |
| `src/floor.cc`, `src/floor.h` | the references: the spinning pool and the engine scenario over bare arrays |
| `src/stress.cc` | the engine scenario as a module: the ktable, the systems, the handler, `cpukiller_core_modules()` |
| `include/korvid/cpukiller/module.h` | the record, `cpukiller_set_bodies`, the headless module list |

The module keeps two module lists of its own (`module_list = false` in `module.lua`): the
generated one with the window for `--window`, and `cpukiller_core_modules()` with only
itself for the headless run, which measures the core alone.

## Behaviour worth knowing

- **Bounce and integrate are ordered by their sets, not by `after`.** Bounce writes `Vel`
  that integrate reads, so bounce runs first; the other way round would be a cycle, since
  "reads what you wrote last tick" cannot be said within a phase.
- **`--quick`** shrinks every scenario and its repeats for a smoke run.
- **The breakdown counts a unit twice.** A unit's zone and its chunks' zones share its
  name, so a zone's sum is the unit's wall time plus its chunks'. Compare a zone with
  itself across thread counts, not with the tick.
- **`--profile FILE`** writes a Chrome trace of the whole run; the engine breakdown is
  read from the same collector.
