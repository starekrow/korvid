// Korvid platform: the io_uring behind IoQueue, as the queue sees it. One submission
// queue, one completion queue, reads only. Raw syscalls over <linux/io_uring.h>; nothing
// links liburing. Every function is a no-op (create returns null) where there is no ring.
#pragma once
#include "korvid/core/types.h"

namespace kv {

struct IoRing;

IoRing* io_ring_create(u32 entries); // null if the kernel refuses or the platform has none
void    io_ring_destroy(IoRing* ring);
// Room for another submission before the next enter.
bool    io_ring_can_push(const IoRing* ring);
// Queues a read of `len` bytes at `offset` into `buf`; `user` comes back on the completion.
void    io_ring_push_read(IoRing* ring, int fd, void* buf, u32 len, u64 offset, u64 user);
// Submits what was pushed; waits for `min_complete` completions (0: none). False on error.
bool    io_ring_enter(IoRing* ring, u32 min_complete);
// Takes one completion: `res` is bytes read or a negative errno. False when none is ready.
bool    io_ring_pop(IoRing* ring, u64* user, i32* res);
u32     io_ring_in_flight(const IoRing* ring);
u32     io_ring_entries(const IoRing* ring);

} // namespace kv
