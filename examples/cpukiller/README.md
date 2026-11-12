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

## The scenarios

1. **Dispatch overhead.** Batches of chunks that do nothing, from 2 to 65,536 chunks:
   what one batch costs to wake the pool and what one chunk costs at scale.
2. **Contention.** Fixed work cut into chunks from tiny to large: the grain below which
   the queue's lock is the bottleneck and the speedup it caps.
3. **Nested batches.** Every outer chunk runs an inner batch: the pool must not deadlock
   and the caller must help, which it does because a waiter works the queue.
4. **Memory under load.** Same-thread allocate and free, remote free (allocated on one
   thread, freed on another), scratch arenas and arrays, each against a warm-pool
   baseline; the size-class allocator's cross-thread paths are what this exercises.
5. **The engine.** The `cpukiller.body` kind (`Pos` with `CompPrev`, `Vel`, `Age`) at
   `--bodies` entities, four systems (integrate in a `parallel_for`, bounce with an event
   per bounce, churn by chunk commands, a reduction) and a handler that eats the events,
   run for `--ticks` ticks at one thread and at `--threads`; the hashes must be equal.
   The breakdown comes from the profiler's collector: the top zones of each run and the
   workers' time split into busy and waiting.

With `--window` the usual stack comes up first (platform, renderer, frame loop), the
pool and memory scenarios run while the window and swapchain are alive, and the engine
ticks are driven through `frameloop_step` with a render every 16 ticks: memory can
behave differently with a GPU's fingers in it.

## What it found

Numbers from the slice 2 memory and job passes on a 16-thread laptop, so the tool has
something to compare against: about 15 us of wake latency per batch and 170 ns per chunk
at scale; grain 1 caps the speedup near 6x of 16 where grain 8 and up reaches about 12x;
the size-class allocator took remote frees from 1.6x to 5.0x of serial and same-thread
work from 2.6x to 3.2x; and in the engine scenario the workers wait about 46% of their
time at level barriers and in the serial residue, which is the number to beat.

## Where things are

| file | what |
|---|---|
| `src/main.cc` | the scenarios, the report, the window mode, `main` |
| `src/stress.cc` | the engine scenario as a module: the kind, the systems, the handler, `cpukiller_core_modules()` |
| `include/korvid/cpukiller/module.h` | the record, `cpukiller_set_bodies`, the headless module list |

The module keeps two module lists of its own (`module_list = false` in `module.lua`): the
generated one with the window for `--window`, and `cpukiller_core_modules()` with only
itself for the headless run, which measures the core alone.

## Behaviour worth knowing

- **Bounce and integrate are ordered by their sets, not by `after`.** Bounce writes `Vel`
  that integrate reads, so bounce runs first; the other way round would be a cycle, since
  "reads what you wrote last tick" cannot be said within a phase.
- **`--quick`** shrinks every scenario for a smoke run.
- **`--profile FILE`** writes a Chrome trace of the whole run; the engine breakdown is
  read from the same collector.
