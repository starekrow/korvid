// Korvid core: thread identity and per-thread flags. Ids are small integers assigned on
// first use (1 = the first thread to ask, normally main); 0 means "no thread".
#pragma once
#include "korvid/core/types.h"

namespace kv {

u32  thread_id();
// A real-time thread (audio) must never touch the general allocator; it asserts.
void thread_set_realtime(bool rt);
bool thread_is_realtime();

} // namespace kv
