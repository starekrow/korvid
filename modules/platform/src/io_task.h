// Korvid platform: IoQueue's task workers, as io.cc sees them (io_task.cc).
#pragma once
#include "korvid/platform/io.h"

namespace kv {

// The pump's part: what the workers finished, onto the completion list.
void io_tasks_complete(IoQueue& q);
// Asks a task that has not completed to stop: it is never started, or sees cancelled().
bool io_tasks_cancel(IoQueue& q, IoEntry& e);
// free()'s part: every task asked to stop, the workers joined, the nodes freed.
void io_tasks_stop(IoQueue& q);

} // namespace kv
