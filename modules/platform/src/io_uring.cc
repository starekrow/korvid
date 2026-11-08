// Korvid platform: the io_uring. Setup maps the submission and completion rings (one
// mapping when the kernel offers IORING_FEAT_SINGLE_MMAP, two otherwise) and the SQE
// array; push fills the next SQE and publishes the tail with a release store; enter is
// one syscall per pump; pop reads completions behind the tail with an acquire load.
#include "io_ring.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"

#if defined(__linux__)

#include <errno.h>
#include <linux/io_uring.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace kv {

struct IoRing {
    int   fd       = -1;
    u32   entries  = 0;
    u32   pushed   = 0; // since the last enter
    u32   in_flight = 0;
    // Submission queue.
    u8*   sq_ptr   = nullptr;
    usize sq_len   = 0;
    u32*  sq_head  = nullptr;
    u32*  sq_tail  = nullptr;
    u32*  sq_mask  = nullptr;
    u32*  sq_array = nullptr;
    io_uring_sqe* sqes = nullptr;
    usize sqes_len = 0;
    // Completion queue.
    u8*   cq_ptr   = nullptr;
    usize cq_len   = 0;
    u32*  cq_head  = nullptr;
    u32*  cq_tail  = nullptr;
    u32*  cq_mask  = nullptr;
    io_uring_cqe* cqes = nullptr;
    bool  single_mmap = false;
};

IoRing* io_ring_create(u32 entries) {
    io_uring_params p{};
    int fd = (int)syscall(__NR_io_uring_setup, entries, &p);
    if (fd < 0) {
        KV_INFO("io", "io_uring unavailable (%s): synchronous reads", strerror(errno));
        return nullptr;
    }
    IoRing* r = alloc_items<IoRing>(mem_general());
    *r = IoRing{};
    r->fd      = fd;
    r->entries = p.sq_entries;
    r->sq_len  = p.sq_off.array + (usize)p.sq_entries * sizeof(u32);
    r->cq_len  = p.cq_off.cqes + (usize)p.cq_entries * sizeof(io_uring_cqe);
    r->single_mmap = (p.features & IORING_FEAT_SINGLE_MMAP) != 0;
    if (r->single_mmap) {
        if (r->cq_len > r->sq_len) r->sq_len = r->cq_len;
        r->cq_len = r->sq_len;
    }
    r->sq_ptr = (u8*)mmap(nullptr, r->sq_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (r->sq_ptr == MAP_FAILED) { r->sq_ptr = nullptr; io_ring_destroy(r); return nullptr; }
    if (r->single_mmap) r->cq_ptr = r->sq_ptr;
    else {
        r->cq_ptr = (u8*)mmap(nullptr, r->cq_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
        if (r->cq_ptr == MAP_FAILED) { r->cq_ptr = nullptr; io_ring_destroy(r); return nullptr; }
    }
    r->sqes_len = (usize)p.sq_entries * sizeof(io_uring_sqe);
    r->sqes = (io_uring_sqe*)mmap(nullptr, r->sqes_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
    if (r->sqes == MAP_FAILED) { r->sqes = nullptr; io_ring_destroy(r); return nullptr; }

    r->sq_head  = (u32*)(r->sq_ptr + p.sq_off.head);
    r->sq_tail  = (u32*)(r->sq_ptr + p.sq_off.tail);
    r->sq_mask  = (u32*)(r->sq_ptr + p.sq_off.ring_mask);
    r->sq_array = (u32*)(r->sq_ptr + p.sq_off.array);
    r->cq_head  = (u32*)(r->cq_ptr + p.cq_off.head);
    r->cq_tail  = (u32*)(r->cq_ptr + p.cq_off.tail);
    r->cq_mask  = (u32*)(r->cq_ptr + p.cq_off.ring_mask);
    r->cqes     = (io_uring_cqe*)(r->cq_ptr + p.cq_off.cqes);
    KV_INFO("io", "io_uring: %u submission entries, %u completion entries", p.sq_entries, p.cq_entries);
    return r;
}

void io_ring_destroy(IoRing* r) {
    if (!r) return;
    if (r->sqes) munmap(r->sqes, r->sqes_len);
    if (r->cq_ptr && !r->single_mmap) munmap(r->cq_ptr, r->cq_len);
    if (r->sq_ptr) munmap(r->sq_ptr, r->sq_len);
    if (r->fd >= 0) close(r->fd);
    free_items(mem_general(), r);
}

bool io_ring_can_push(const IoRing* r) {
    u32 head = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
    return *r->sq_tail - head < r->entries;
}

void io_ring_push_write(IoRing* r, int fd, const void* buf, u32 len, u64 offset, u64 user) {
    u32 tail = *r->sq_tail;
    u32 idx  = tail & *r->sq_mask;
    io_uring_sqe& s = r->sqes[idx];
    memset(&s, 0, sizeof s);
    s.opcode    = IORING_OP_WRITE;
    s.fd        = fd;
    s.addr      = (u64)(usize)buf;
    s.len       = len;
    s.off       = offset;
    s.user_data = user;
    r->sq_array[idx] = idx;
    __atomic_store_n(r->sq_tail, tail + 1, __ATOMIC_RELEASE);
    ++r->pushed;
}

void io_ring_push_read(IoRing* r, int fd, void* buf, u32 len, u64 offset, u64 user) {
    u32 tail = *r->sq_tail;
    u32 idx  = tail & *r->sq_mask;
    io_uring_sqe& s = r->sqes[idx];
    memset(&s, 0, sizeof s);
    s.opcode    = IORING_OP_READ;
    s.fd        = fd;
    s.addr      = (u64)(usize)buf;
    s.len       = len;
    s.off       = offset;
    s.user_data = user;
    r->sq_array[idx] = idx;
    __atomic_store_n(r->sq_tail, tail + 1, __ATOMIC_RELEASE);
    ++r->pushed;
}

bool io_ring_enter(IoRing* r, u32 min_complete) {
    if (r->pushed == 0 && min_complete == 0) return true;
    u32 flags = min_complete ? IORING_ENTER_GETEVENTS : 0;
    long n = syscall(__NR_io_uring_enter, r->fd, r->pushed, min_complete, flags, nullptr, 0);
    if (n < 0) {
        if (errno == EINTR) return true; // nothing submitted; the next pump tries again
        KV_ERROR("io", "io_uring_enter: %s", strerror(errno));
        return false;
    }
    r->in_flight += (u32)n;
    r->pushed    -= (u32)n;
    return true;
}

bool io_ring_pop(IoRing* r, u64* user, i32* res) {
    u32 head = *r->cq_head;
    u32 tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);
    if (head == tail) return false;
    const io_uring_cqe& c = r->cqes[head & *r->cq_mask];
    *user = c.user_data;
    *res  = c.res;
    __atomic_store_n(r->cq_head, head + 1, __ATOMIC_RELEASE);
    --r->in_flight;
    return true;
}

u32 io_ring_in_flight(const IoRing* r) { return r->in_flight; }
u32 io_ring_entries(const IoRing* r) { return r->entries; }

} // namespace kv

#else

namespace kv {

IoRing* io_ring_create(u32) { return nullptr; }
void    io_ring_destroy(IoRing*) {}
bool    io_ring_can_push(const IoRing*) { return false; }
void    io_ring_push_read(IoRing*, int, void*, u32, u64, u64) {}
void    io_ring_push_write(IoRing*, int, const void*, u32, u64, u64) {}
bool    io_ring_enter(IoRing*, u32) { return false; }
bool    io_ring_pop(IoRing*, u64*, i32*) { return false; }
u32     io_ring_in_flight(const IoRing*) { return 0; }
u32     io_ring_entries(const IoRing*) { return 0; }

} // namespace kv

#endif
