// Korvid core: monotonic clock and sleep. Implemented per platform under src/<platform>/.
#pragma once
#include "korvid/core/types.h"

namespace kv {

f64  time_now();              // seconds, monotonic, arbitrary origin
void time_sleep(f64 seconds); // coarse; for pacing a loop that has nothing to present

// The raw clock the profiler records: the highest resolution the platform has, monotonic.
u64  os_ticks();
u64  os_ticks_per_second();
// This thread's CPU time in the same units, where the platform has it; 0 where it does not.
// A system call on most platforms: the profiler records it only when asked.
u64  os_thread_cpu_ticks();

} // namespace kv
