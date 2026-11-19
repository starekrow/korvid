// Korvid core: thread identity and per-thread flags.
//
// Ids are small integers assigned on first use (1 = the first thread to ask, normally
// main) and never reused while the process runs; 0 means "no thread", so a field holding
// an id can start at 0. The real-time flag marks a thread that must never wait on the
// allocator (the audio callback's): debug and dev builds abort when such a thread reaches
// the general heap or makes an arena commit pages.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// The calling thread's id, from 1; the first call on a thread assigns it.
u32  thread_id();
// Logical processors online, at least 1. Per platform.
u32  os_cpu_count();
// Marks or clears the calling thread as real-time (audio). A real-time thread must never
// touch the general allocator; debug and dev builds assert it.
void thread_set_realtime(bool rt);
// Whether the calling thread is marked real-time.
bool thread_is_realtime();

} // namespace kv
