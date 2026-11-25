// Korvid platform: the async ring behind IoQueue on Android, over a few threads of its own.
// Android has io_uring in its kernel and refuses it to apps (seccomp answers an app's
// io_uring_setup with SIGSYS), so the ring is the shape of Linux's with workers as the
// kernel, as the Apple family's is over Grand Central Dispatch (apple/io_ring_apple.cc):
// enter hands each request pushed since the last enter to the workers, one does the
// blocking pread or pwrite straight into the queue's buffer and posts the result to the
// completion list, pop takes from it. The queue's thread never blocks on a read.
//
// The ring's thread is one (the queue's): push, enter, pop and the counts. Workers touch
// only their request, the submission list and the completion list, which one lock guards;
// they sleep on one condition and enter's wait is another.
#include "../io_ring.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"

#if defined(__ANDROID__)

#include <errno.h>
#include <pthread.h>
#include <unistd.h>

namespace kv {

namespace {

struct RingOp {
    int   fd;
    void* buf;
    u32   len;
    bool  write;
    u64   offset;
    u64   user;
    u32   next_free;
};

struct RingCompletion { u64 user; i32 res; u32 op; };

constexpr u32 kNone    = 0xFFFFFFFFu;
constexpr u32 kWorkers = 4; // blocked in the kernel most of their lives: storage's queue depth, not cores

} // namespace

struct IoRing {
    u32             entries = 0;
    RingOp*         ops = nullptr;         // `entries` of them, free or in flight
    u32             free_head = kNone;
    u32*            pending = nullptr;     // ops pushed since the last enter
    u32             pending_count = 0;
    u32             in_flight = 0;         // pushed and not yet popped
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; // the two lists below, and stop
    pthread_cond_t  work = PTHREAD_COND_INITIALIZER;  // a submission, or stop
    pthread_cond_t  done = PTHREAD_COND_INITIALIZER;  // a completion
    u32*            submitted = nullptr;   // a ring of `entries`: handed to the workers
    u32             sub_head = 0, sub_tail = 0;
    RingCompletion* completions = nullptr; // a ring of `entries`
    u32             head = 0, tail = 0;    // taken at head, posted at tail
    bool            stop = false;
    pthread_t       workers[kWorkers];
    u32             worker_count = 0;
};

namespace {

void* ring_worker(void* ctx) {
    IoRing* r = (IoRing*)ctx;
    pthread_mutex_lock(&r->lock);
    for (;;) {
        while (!r->stop && r->sub_head == r->sub_tail) pthread_cond_wait(&r->work, &r->lock);
        if (r->sub_head == r->sub_tail) break; // stopping, and nothing left to do
        u32 i = r->submitted[r->sub_head++ % r->entries];
        pthread_mutex_unlock(&r->lock);
        RingOp& op = r->ops[i];
        ssize_t n;
        do {
            n = op.write ? pwrite(op.fd, op.buf, op.len, (off_t)op.offset) : pread(op.fd, op.buf, op.len, (off_t)op.offset);
        } while (n < 0 && errno == EINTR);
        i32 res = n < 0 ? -errno : (n > 0x7FFFFFFF ? 0x7FFFFFFF : (i32)n);
        pthread_mutex_lock(&r->lock);
        r->completions[r->tail++ % r->entries] = RingCompletion{op.user, res, i};
        pthread_cond_signal(&r->done);
    }
    pthread_mutex_unlock(&r->lock);
    return nullptr;
}

void ring_push(IoRing* r, Handle fd, void* buf, u32 len, u64 offset, u64 user, bool write) {
    KV_ASSERT(r->free_head != kNone); // the queue asks can_push first
    u32 i = r->free_head;
    RingOp& op = r->ops[i];
    r->free_head = op.next_free;
    op = RingOp{(int)os_handle_value(fd), buf, len, write, offset, user, kNone};
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
    r->submitted = alloc_items<u32>(mem_general(), entries);
    r->completions = alloc_items<RingCompletion>(mem_general(), entries);
    if (!r->ops || !r->pending || !r->submitted || !r->completions) { io_ring_destroy(r); return nullptr; }
    for (u32 i = 0; i < entries; ++i) r->ops[i].next_free = i + 1 < entries ? i + 1 : kNone;
    r->free_head = 0;
    for (u32 i = 0; i < kWorkers; ++i) {
        if (pthread_create(&r->workers[r->worker_count], nullptr, ring_worker, r) != 0) break;
        ++r->worker_count;
    }
    if (!r->worker_count) { io_ring_destroy(r); return nullptr; }
    KV_INFO("io", "thread ring: %u entries, pread and pwrite on %u threads", entries, r->worker_count);
    return r;
}

void io_ring_destroy(IoRing* r) {
    if (!r) return;
    // The workers finish what they hold (IoQueue::free waited for every completion already).
    pthread_mutex_lock(&r->lock);
    r->stop = true;
    pthread_cond_broadcast(&r->work);
    pthread_mutex_unlock(&r->lock);
    for (u32 i = 0; i < r->worker_count; ++i) pthread_join(r->workers[i], nullptr);
    if (r->ops) free_items(mem_general(), r->ops, r->entries);
    if (r->pending) free_items(mem_general(), r->pending, r->entries);
    if (r->submitted) free_items(mem_general(), r->submitted, r->entries);
    if (r->completions) free_items(mem_general(), r->completions, r->entries);
    free_items(mem_general(), r);
}

bool io_ring_can_push(const IoRing* r) { return r && r->in_flight < r->entries; }

void io_ring_push_read(IoRing* r, Handle fd, void* buf, u32 len, u64 offset, u64 user) { ring_push(r, fd, buf, len, offset, user, false); }

void io_ring_push_write(IoRing* r, Handle fd, const void* buf, u32 len, u64 offset, u64 user) {
    ring_push(r, fd, (void*)buf, len, offset, user, true); // a write only reads it
}

bool io_ring_enter(IoRing* r, u32 min_complete) {
    if (!r) return false;
    pthread_mutex_lock(&r->lock);
    for (u32 i = 0; i < r->pending_count; ++i) r->submitted[r->sub_tail++ % r->entries] = r->pending[i];
    if (r->pending_count > 1) pthread_cond_broadcast(&r->work);
    else if (r->pending_count) pthread_cond_signal(&r->work);
    r->pending_count = 0;
    if (min_complete > r->in_flight) min_complete = r->in_flight;
    while (r->tail - r->head < min_complete) pthread_cond_wait(&r->done, &r->lock);
    pthread_mutex_unlock(&r->lock);
    return true;
}

bool io_ring_pop(IoRing* r, u64* user, i32* res) {
    if (!r) return false;
    pthread_mutex_lock(&r->lock);
    bool any = r->head != r->tail;
    RingCompletion c{};
    if (any) c = r->completions[r->head++ % r->entries];
    pthread_mutex_unlock(&r->lock);
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
const char* io_ring_name(const IoRing* r) { return r ? "threads" : "sync"; }

} // namespace kv

#endif
