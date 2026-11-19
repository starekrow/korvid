// Korvid core: the job pool: worker threads that run one function over a range, in
// chunks, with the calling thread working alongside them.
//
// One pool of workers serves the process, fed from a single FIFO of chunks. A batch is a
// function over [0, count) cut into chunks of `grain`; jobs_run queues every chunk, then
// the caller works the queue itself until its batch is done, so a batch started from
// inside another batch's chunk makes progress on the thread that is waiting for it.
// Nothing blocks a thread that could be working. The engine starts the pool in
// engine_init and stops it in engine_shutdown.
//
// Systems use the pool through parallel_for and parallel_reduce (phase.h), which give each
// chunk its own event and command buffers and merge them in chunk order, so the result
// does not depend on which thread ran what. jobs_run is the layer below, for engine code
// with nothing to emit (modules/renderer-cpu/src/frame.cc, the tiles):
//
//   jobs_run(count, 1, draw_tiles, &j, "cpu.tiles");   // one tile to draw a chunk
//
// Inside a chunk: a worker resets its scratch arena before each chunk it takes, so
// scratch there lasts until the chunk returns and no longer. Attribution follows the
// chunk: the thread-local memory owner is the one that called jobs_run, so a bare
// mem_alloc inside a job lands on the right module. Each chunk is a profiler zone named
// after its batch.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// A chunk's work: the batch's range [begin, end), `chunk` its index (from 0, below
// jobs_chunks(count, grain)), `user` as given to jobs_run. Chunks run on any pool thread
// or the caller, at the same time as each other and in no fixed order.
typedef void (*JobFn)(u32 chunk, u32 begin, u32 end, void* user);

// ---- internal: a batch in flight, as the queue sees it --------------------------------
struct JobBatch {
    JobFn       fn;
    void*       user;
    u32         count, grain, chunks;
    u16         owner;      // mem owner (module index + 1) for attribution inside the chunks
    u32         done;       // atomic: chunks finished
    const char* name;       // the profiler's name for the chunks
};

// ---- the pool -------------------------------------------------------------------------
// Starts `workers` threads (0 is fine: every batch then runs on the caller). Idempotent
// for the same count; a different count stops the old threads first. engine_init calls it
// with one fewer than the engine's thread count (--threads; by default every logical
// processor, at most 64). False, logged, when a thread does not start; the pool is then
// down. With no memory for the threads it logs, runs with 0 workers and returns true.
bool jobs_init(u32 workers);
// Stops and joins the workers and frees the queue; nothing if the pool is down. After it,
// jobs_run runs every batch on the caller.
void jobs_shutdown();
// Worker threads running, not counting the threads that call jobs_run; 0 when the pool is
// down.
u32  jobs_worker_count();

// ---- batches --------------------------------------------------------------------------
// Runs fn over [0, count) in chunks of grain and returns when every chunk has run. The
// caller takes chunks too. Chunk i covers [i * grain, min(count, (i + 1) * grain)). A count
// or grain of 0 runs nothing; with no workers, one chunk, or a queue that cannot grow,
// every chunk runs on the caller, in order. May be called from inside a chunk. `name`, a
// static string, names the chunks in the profiler.
void jobs_run(u32 count, u32 grain, JobFn fn, void* user, const char* name = "job");
// How many chunks jobs_run would make.
inline u32 jobs_chunks(u32 count, u32 grain) { return grain ? (count + grain - 1) / grain : 0; }

// ---- statistics -----------------------------------------------------------------------
// What the pool has been doing, for profiling. Counters are process-wide and relaxed.
struct JobStats {
    u64 batches;       // jobs_run calls that went to the queue (not the inline path)
    u64 inline_batches;// ran entirely on the caller: no workers, one chunk, or no room to queue
    u64 tasks;         // chunks run from the queue
    u64 tasks_by_caller; // of those, run by the thread that was waiting for them
    u64 helper_waits;  // a waiting caller found the queue empty and slept
    u64 worker_waits;  // a worker found the queue empty and slept
};
// The counters since the last reset, each read on its own.
JobStats jobs_stats();
// Zeroes the counters. A plain write, so call it between batches (examples/cpukiller
// does, before each case it measures).
void     jobs_stats_reset();

} // namespace kv
