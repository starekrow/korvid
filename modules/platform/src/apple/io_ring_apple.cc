// Korvid platform: the async ring behind IoQueue on the Apple family, over Grand Central
// Dispatch. Neither macOS nor iOS has a kernel ring: POSIX AIO allows 16 requests a process
// (kern.aioprocmax) and dispatch_io reads into buffers of its own, a copy per read. So the
// ring is the shape of Linux's and Windows', with GCD's workers as the kernel: enter hands
// each request pushed since the last enter to a concurrent dispatch queue, a worker does
// the blocking pread or pwrite straight into the queue's buffer and posts the result to the
// completion list, pop takes from it. The queue's thread never blocks on a read; GCD grows
// its pool while workers wait in the kernel, as it does for blocking work.
//
// The ring's thread is one (the queue's): push, enter, pop and the counts. Workers touch only
// their request and the completion list, which a lock guards; enter's wait is a semaphore,
// one signal per completion.
#include "../io_ring.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"

#if defined(__APPLE__)

#include <dispatch/dispatch.h>
#include <errno.h>
#include <os/lock.h>
#include <unistd.h>

namespace kv {

namespace {

struct RingOp {
    IoRing*     ring;
    int         fd;
    void*       buf;
    u32         len;
    bool        write;
    u64         offset;
    u64         user;
    u32         next_free;
};

struct RingCompletion { u64 user; i32 res; u32 op; };

constexpr u32 kNone = 0xFFFFFFFFu;

} // namespace

struct IoRing {
    u32                  entries = 0;
    RingOp*              ops = nullptr;          // `entries` of them, free or in flight
    u32                  free_head = kNone;
    u32*                 pending = nullptr;      // ops pushed since the last enter
    u32                  pending_count = 0;
    u32                  in_flight = 0;          // pushed and not yet popped
    dispatch_queue_t     queue = nullptr;
    dispatch_semaphore_t done = nullptr;         // a signal per completion
    os_unfair_lock       lock = OS_UNFAIR_LOCK_INIT; // guards the completion list
    RingCompletion*      completions = nullptr;  // a ring of `entries`
    u32                  head = 0, tail = 0;     // taken at head, posted at tail
};

namespace {

// A worker: the blocking call, then the result to the list.
void ring_work(void* ctx) {
    RingOp& op = *(RingOp*)ctx;
    ssize_t n;
    do {
        n = op.write ? pwrite(op.fd, op.buf, op.len, (off_t)op.offset) : pread(op.fd, op.buf, op.len, (off_t)op.offset);
    } while (n < 0 && errno == EINTR);
    i32 res = n < 0 ? -errno : (n > 0x7FFFFFFF ? 0x7FFFFFFF : (i32)n);
    IoRing* r = op.ring;
    os_unfair_lock_lock(&r->lock);
    r->completions[r->tail++ % r->entries] = RingCompletion{op.user, res, (u32)(&op - r->ops)};
    os_unfair_lock_unlock(&r->lock);
    dispatch_semaphore_signal(r->done);
}

void ring_push(IoRing* r, KHandle fd, void* buf, u32 len, u64 offset, u64 user, bool write) {
    KV_ASSERT(r->free_head != kNone); // the queue asks can_push first
    u32 i = r->free_head;
    RingOp& op = r->ops[i];
    r->free_head = op.next_free;
    op = RingOp{r, (int)fd.os(), buf, len, write, offset, user, kNone};
    r->pending[r->pending_count++] = i;
    ++r->in_flight;
}

} // namespace

IoRing* io_ring_create(u32 entries) {
    if (!entries) return nullptr;
    IoRing* r = alloc_items<IoRing>(mem_general());
    *r = IoRing{};
    r->entries = entries;
    r->ops = alloc_items<RingOp>(mem_general(), entries);
    r->pending = alloc_items<u32>(mem_general(), entries);
    r->completions = alloc_items<RingCompletion>(mem_general(), entries);
    if (!r->ops || !r->pending || !r->completions) { io_ring_destroy(r); return nullptr; }
    for (u32 i = 0; i < entries; ++i) r->ops[i].next_free = i + 1 < entries ? i + 1 : kNone;
    r->free_head = 0;
    // User-initiated: a game is waiting on these (a level, a texture), more than utility work.
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_USER_INITIATED, 0);
    r->queue = dispatch_queue_create("korvid.io", attr);
    r->done = dispatch_semaphore_create(0);
    if (!r->queue || !r->done) { io_ring_destroy(r); return nullptr; }
    KV_INFO("io", "dispatch ring: %u entries, pread and pwrite on a concurrent dispatch queue", entries);
    return r;
}

void io_ring_destroy(IoRing* r) {
    if (!r) return;
    // The queue drains what it holds (IoQueue::free waited for every completion already).
    if (r->queue) dispatch_release(r->queue);
    if (r->done) dispatch_release(r->done);
    if (r->ops) free_items(mem_general(), r->ops, r->entries);
    if (r->pending) free_items(mem_general(), r->pending, r->entries);
    if (r->completions) free_items(mem_general(), r->completions, r->entries);
    free_items(mem_general(), r);
}

bool io_ring_can_push(const IoRing* r) { return r && r->in_flight < r->entries; }

void io_ring_push_read(IoRing* r, KHandle fd, void* buf, u32 len, u64 offset, u64 user) { ring_push(r, fd, buf, len, offset, user, false); }

void io_ring_push_write(IoRing* r, KHandle fd, const void* buf, u32 len, u64 offset, u64 user) {
    ring_push(r, fd, (void*)buf, len, offset, user, true); // a write only reads it
}

bool io_ring_enter(IoRing* r, u32 min_complete) {
    if (!r) return false;
    for (u32 i = 0; i < r->pending_count; ++i) dispatch_async_f(r->queue, &r->ops[r->pending[i]], ring_work);
    r->pending_count = 0;
    if (min_complete > r->in_flight) min_complete = r->in_flight;
    // Wait until that many are ready to pop. A signal can belong to a completion an earlier
    // pop took already, so the count is the list's, and the semaphore only the wake-up.
    for (;;) {
        os_unfair_lock_lock(&r->lock);
        u32 ready = r->tail - r->head;
        os_unfair_lock_unlock(&r->lock);
        if (ready >= min_complete) return true;
        dispatch_semaphore_wait(r->done, DISPATCH_TIME_FOREVER);
    }
}

bool io_ring_pop(IoRing* r, u64* user, i32* res) {
    if (!r) return false;
    os_unfair_lock_lock(&r->lock);
    bool any = r->head != r->tail;
    RingCompletion c{};
    if (any) c = r->completions[r->head++ % r->entries];
    os_unfair_lock_unlock(&r->lock);
    if (!any) return false;
    // The worker is done with its op: the slot is free.
    r->ops[c.op].next_free = r->free_head;
    r->free_head = c.op;
    *user = c.user;
    *res = c.res;
    --r->in_flight;
    return true;
}

u32 io_ring_in_flight(const IoRing* r) { return r ? r->in_flight : 0; }
u32 io_ring_entries(const IoRing* r) { return r ? r->entries : 0; }
const char* io_ring_name(const IoRing* r) { return r ? "dispatch" : "sync"; }

} // namespace kv

#endif
