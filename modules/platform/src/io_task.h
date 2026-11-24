// Korvid platform: IoQueue's task workers, as io.cc sees them (io_task.cc).
#pragma once
#include "korvid/platform/io.h"
#include "io_internal.h"

namespace kv {

// The pump's part: what the workers finished, onto the completion list.
void io_tasks_complete(IoQueue& q);
// Asks a task that has not completed to stop: it is never started, or sees cancelled().
bool io_tasks_cancel(IoQueue& q, IoEntry& e);
// free()'s part: every task asked to stop, the workers joined, the nodes freed.
void io_tasks_stop(IoQueue& q);
// A stat_file entry onto the workers, in a lane of its own that no limit applies to. False
// if there is no memory for its node.
bool io_tasks_stat(IoQueue& q, Handle h, IoEntry& e);

// A stat now, on the calling thread, as stat_file answers it: 0 with *out filled (all false
// for a path where nothing is), or the errno of a failure. io.cc.
i32 io_stat_errno(const char* path, IoStat* out);

} // namespace kv
