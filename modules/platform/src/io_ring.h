// Korvid platform: the async ring behind IoQueue, as the queue sees it: io_uring on Linux
// (io_uring.cc, raw syscalls over <linux/io_uring.h>; nothing links liburing), Windows I/O
// rings on Windows 11 (win32/io_ring_win32.cc), Grand Central Dispatch's workers on the
// Apple family (apple/io_ring_apple.cc), a few threads of our own on Android, whose apps
// are refused io_uring (android/io_ring_android.cc). One submission queue, one completion queue,
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
// A stat as one ring operation, where the ring has one (io_uring's IORING_OP_STATX; the
// others answer false, and the queue sends stats to its task workers). `buf` is
// io_ring_stat_size() bytes for the kernel's answer; it and `path` stay put until the
// completion, whose `res` is 0 or a negative errno. io_ring_stat_read turns the answer
// into an IoStat. All in io_uring.cc, the stubs for every other ring included.
bool    io_ring_can_stat(const IoRing* ring);
usize   io_ring_stat_size();
void    io_ring_push_stat(IoRing* ring, const char* path, void* buf, u64 user);
void    io_ring_stat_read(const void* buf, IoStat* out);
u32     io_ring_in_flight(const IoRing* ring);
u32     io_ring_entries(const IoRing* ring);
// "io_uring", "ioring", "dispatch", "threads", or "sync" for none: what the queue logs and reports.
const char* io_ring_name(const IoRing* ring);

} // namespace kv
