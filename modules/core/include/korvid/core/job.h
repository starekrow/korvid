// Korvid core: the job system. One pool of worker threads for the process, fed from a
// single FIFO of chunks. A batch is a function over [0, count) cut into chunks of
// `grain`; run_batch queues every chunk, then the caller works the queue itself until
// its batch is done, so a batch started from inside another batch's chunk makes progress
// on the thread that is waiting for it. Nothing blocks a thread that could be working.
//
// Workers reset their scratch arena at the start of every top-level chunk: scratch is
// job lifetime there. Attribution follows the chunk: the thread-local owner is set from
// the batch so a bare mem_alloc inside a job lands on the right module.
#pragma once
#include "korvid/core/types.h"

namespace kv {

typedef void (*JobFn)(u32 chunk, u32 begin, u32 end, void* user);

struct JobBatch {
    JobFn       fn;
    void*       user;
    u32         count, grain, chunks;
    u16         owner;      // mem owner (module index + 1) for attribution inside the chunks
    u32         done;       // atomic: chunks finished
    const char* name;       // the profiler's name for the chunks
};

// Starts `workers` threads (0 is fine: every batch then runs on the caller). Idempotent
// for the same count; a different count stops the old threads first.
bool jobs_init(u32 workers);
void jobs_shutdown();
u32  jobs_worker_count();

// Runs fn over [0, count) in chunks of grain and returns when every chunk has run. The
// caller takes chunks too. Chunk i covers [i * grain, min(count, (i + 1) * grain)).
void jobs_run(u32 count, u32 grain, JobFn fn, void* user, const char* name = "job");
// How many chunks jobs_run would make.
inline u32 jobs_chunks(u32 count, u32 grain) { return grain ? (count + grain - 1) / grain : 0; }

// What the pool has been doing, for profiling. Counters are process-wide and relaxed.
struct JobStats {
    u64 batches;       // jobs_run calls that went to the queue (not the inline path)
    u64 inline_batches;// ran entirely on the caller: no workers, or one chunk
    u64 tasks;         // chunks run from the queue
    u64 tasks_by_caller; // of those, run by the thread that was waiting for them
    u64 helper_waits;  // a waiting caller found the queue empty and slept
    u64 worker_waits;  // a worker found the queue empty and slept
};
JobStats jobs_stats();
void     jobs_stats_reset();

} // namespace kv
