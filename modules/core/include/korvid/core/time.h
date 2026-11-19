// Korvid core: the monotonic clock and sleep.
//
// Two views of one clock, which never goes back and has an arbitrary origin, so only
// differences between readings mean anything. time_now() is seconds as an f64, for frame
// timing and pacing. os_ticks() is the raw counter the profiler records, converted to
// seconds with os_ticks_per_second(): nanoseconds on POSIX (CLOCK_MONOTONIC), the
// performance counter on Windows. The sim's time is ticks (tick.h), which are the same on
// every run; this clock is not. Implemented per platform under src/<platform>/time.cc.
#pragma once
#include "korvid/core/types.h"

namespace kv {

f64  time_now();              // seconds, monotonic, arbitrary origin
// Sleeps the calling thread about `seconds`; 0 or less returns at once. Coarse: for
// pacing a loop that has nothing to present.
void time_sleep(f64 seconds);

// The raw clock the profiler records: the highest resolution the platform has, monotonic.
u64  os_ticks();
// os_ticks per second: 1000000000 on POSIX, the performance counter's frequency on Windows.
u64  os_ticks_per_second();
// This thread's CPU time in the same units, where the platform has it; 0 where it does not.
// A system call on most platforms: the profiler records it only when asked.
u64  os_thread_cpu_ticks();

} // namespace kv
