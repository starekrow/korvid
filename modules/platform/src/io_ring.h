// Korvid platform: the async ring behind IoQueue, as the queue sees it: io_uring on Linux
// (io_uring.cc, raw syscalls over <linux/io_uring.h>; nothing links liburing), Windows I/O
// rings on Windows 11 (win32/io_ring_win32.cc). One submission queue, one completion queue,
// reads and writes. Every function is a no-op (create returns null) where there is no ring.
#pragma once
#include "korvid/platform/io.h"

namespace kv {

struct IoRing;

IoRing* io_ring_create(u32 entries); // null if the kernel refuses or the platform has none
void    io_ring_destroy(IoRing* ring);
// Room for another submission before the next enter.
bool    io_ring_can_push(const IoRing* ring);
// Queues a read of `len` bytes at `offset` into `buf`; `user` comes back on the completion.
void    io_ring_push_read(IoRing* ring, OsFile fd, void* buf, u32 len, u64 offset, u64 user);
// Queues a write of `len` bytes from `buf` at `offset`.
void    io_ring_push_write(IoRing* ring, OsFile fd, const void* buf, u32 len, u64 offset, u64 user);
// Submits what was pushed; waits for `min_complete` completions (0: none). False on error.
bool    io_ring_enter(IoRing* ring, u32 min_complete);
// Takes one completion: `res` is bytes read or a negative errno. False when none is ready.
bool    io_ring_pop(IoRing* ring, u64* user, i32* res);
u32     io_ring_in_flight(const IoRing* ring);
u32     io_ring_entries(const IoRing* ring);
// "io_uring", "ioring", or "sync" for none: what the queue logs and reports.
const char* io_ring_name(const IoRing* ring);

} // namespace kv
