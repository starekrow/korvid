// cpukiller's references: what the same work costs with the engine taken away, so every
// number in the table has something to stand against.
//
// The floor pool is the least a pool can do: workers that never sleep, spinning on one
// atomic word, and a batch whose chunks are claimed with one fetch-add each. No lock, no
// queue, no wake-up, no profiler zone, no memory attribution, no nesting. It burns every
// core while it is up, which is why nothing ships this way; it is here as the bound a real
// pool is measured against, not as a design to copy.
//
// The plain sim is the engine scenario's four systems (stress.cc) over bare arrays: no
// ktable, no handles, no events, no commands, no previous-tick copy, no state hash. Run
// serially it prices the engine's bookkeeping on one thread; run on the floor pool it is
// what a hand-rolled parallel loop gets, with nothing holding its result to one answer.
#pragma once
#include "korvid/core/job.h"

namespace kv {

// Starts `workers` spinning threads (0 is fine). False when a thread does not start.
bool floor_init(u32 workers);
// Stops and joins them.
void floor_shutdown();
// jobs_run's contract, less everything above: fn over [0, count) in chunks of grain, the
// caller working too, back when every chunk has run. Not from inside a chunk.
void floor_run(u32 count, u32 grain, JobFn fn, void* user);

struct PlainSim;
// `bodies` bodies in the box stress.cc uses, from a fixed seed. Null with no memory.
PlainSim* plain_create(u32 bodies);
void      plain_destroy(PlainSim* s);
// One tick: bounce, integrate, churn, energy, with the engine scenario's grains. On the
// floor pool when `parallel`, else on the caller.
void      plain_tick(PlainSim* s, bool parallel);
// Bounces counted so far, so the work cannot be optimized away and two runs can be compared.
u64       plain_bounces(const PlainSim* s);

} // namespace kv
