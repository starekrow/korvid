// Remote frees while the owner allocates: producers allocate without pause and hand every
// block to a consumer on another thread, which checks it and frees it, so every free is
// remote and lands while the page's owner is still taking blocks from it. A page that drops
// off its owner's notify stack keeps its freed blocks out of reach, and the owner carves new
// pages instead, without end (slice 7 found this; see docs/design/macos-port-notes.md). A
// second round on new threads adopts the first round's orphaned pages, which must serve
// again.
//
// The trigger needs two things at once. Frees into the page the owner is allocating from:
// the owner drains that page's remote list in page_take while the page is still queued on
// its notify stack, and the next free there pushes it again. And many pages of a class on
// the stack, so that the second push has something below it to cut off. So the blocks go
// in two tiers. Even blocks are freed exactly kSlow blocks later: a constant working set of
// many pages, a few dozen blocks to a page in these large classes. Odd blocks are freed as
// soon as the consumer gets to them, with the producer never more than kFastLag ahead: the
// traffic into the active page. Whichever thread is faster, the blocks in flight stay
// within kFastLag of kSlow / 2 from the first few thousand on, in any config on any
// machine, so the pages they need have a ceiling, and a stranding allocator passes it many
// times over (the bound and the numbers are in test_remote_free).
#include "korvid/core/atomic.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_thread.h"
#include "korvid/core/test.h"

namespace {

using namespace kv;

constexpr u32 kPairs   = 3;
constexpr u32 kSlow    = 4096;          // an even block is freed this many blocks after it
constexpr u32 kFastLag = 128;           // the producer runs at most this far past the next odd block to free
constexpr u32 kSlots   = 2 * kSlow;     // the ring's room: a live block's slot is never reused
constexpr u32 kBlocks  = 400000;        // per producer per round, even

// One producer, one consumer. The producer owns head; the consumer owns fast (the next odd
// block to free) and slow (the next even one).
struct Pair {
    void* ring[kSlots];
    u32   head = 0;
    u32   fast = 1;
    u32   slow = 0;
    u32   bad  = 0; // blocks the consumer found not as the producer left them
};

u32 block_size(u32 k) { return 512 * (1 + (k / 2) % 4); } // the 512, 1024, 1536 and 2048 classes, in both tiers

void producer(void* arg) {
    Pair& p = *(Pair*)arg;
    for (u32 k = 0; k < kBlocks; ++k) {
        for (;;) {
            u32 fast = atomic_load(&p.fast, MO_ACQUIRE), slow = atomic_load(&p.slow, MO_ACQUIRE);
            u32 oldest = fast < slow ? fast : slow;
            if (k <= fast + kFastLag && k < oldest + kSlots) break; // else the consumer frees first
        }
        u32 size = block_size(k);
        u8* b = (u8*)mem_alloc(size);
        b[0] = b[size / 2] = b[size - 1] = (u8)k;
        p.ring[k % kSlots] = b;
        atomic_store(&p.head, k + 1, MO_RELEASE);
    }
}

void take(Pair& p, u32 k) {
    u8* b = (u8*)p.ring[k % kSlots];
    u32 size = block_size(k);
    // A block handed out twice, or one the allocator wrote into while it was live.
    if (b[0] != (u8)k || b[size / 2] != (u8)k || b[size - 1] != (u8)k) ++p.bad;
    mem_free(b);
}

void consumer(void* arg) {
    Pair& p = *(Pair*)arg;
    u32 fast = 1, slow = 0;
    while (fast < kBlocks || slow < kBlocks) {
        u32 head = atomic_load(&p.head, MO_ACQUIRE);
        if (fast < kBlocks && head > fast) {
            take(p, fast);
            fast += 2;
            atomic_store(&p.fast, fast, MO_RELEASE);
        } else if (slow < kBlocks && head > slow && (head >= slow + kSlow || head == kBlocks)) {
            take(p, slow);
            slow += 2;
            atomic_store(&p.slow, slow, MO_RELEASE);
        }
    }
}

// One round on fresh threads, which exit at the end and leave their pages orphaned for the
// next round to adopt. The blocks the consumers found damaged.
u32 run_round(Pair* pairs) {
    OsThread threads[2 * kPairs];
    for (u32 i = 0; i < kPairs; ++i) {
        pairs[i] = Pair();
        os_thread_start(&threads[2 * i], producer, &pairs[i]);
        os_thread_start(&threads[2 * i + 1], consumer, &pairs[i]);
    }
    for (OsThread& t : threads) os_thread_join(&t);
    u32 bad = 0;
    for (u32 i = 0; i < kPairs; ++i) bad += pairs[i].bad;
    return bad;
}

} // namespace

void test_remote_free(TestCtx& c) {
    c.begin("remote free");
    static Pair pairs[kPairs];
    MemStats base = mem_stats();
    u64 pages0 = mem_small_pages();

    // The ceiling. The slow tier holds kSlow / 2 blocks a pair, a quarter of them in each
    // class: 512 blocks at 31 to 120 to a page, about 42 pages a pair. Fast blocks share
    // those pages, about as many again, so about 85 a pair, 255 for three; measured 250
    // to 260, reached within a million blocks or so (later in a debug build: the peak comes
    // from the rare moment the owner finds no room before the fast frees come back, and
    // creeps up to the ceiling). Round two adopts round one's pages. A stranding allocator
    // carved 7,000 to 9,000 in round one alone (measured, every config), so twice the
    // ceiling for both rounds tells the two apart with room on both sides.
    constexpr u64 kBound = 512;

    KV_CHECK(c, run_round(pairs) == 0);
    KV_CHECK(c, mem_stats().live_allocs == base.live_allocs);
    u64 pages1 = mem_small_pages();

    // New threads adopt the orphans; a page queued on its dead owner's notify stack must
    // still reach its new owner.
    KV_CHECK(c, run_round(pairs) == 0);
    KV_CHECK(c, mem_stats().live_allocs == base.live_allocs && mem_stats().live_bytes == base.live_bytes);
    u64 pages2 = mem_small_pages();
    KV_CHECK(c, pages2 - pages0 <= kBound);
    KV_INFO("test", "remote frees: %llu pages carved in round one, %llu in round two (bound %llu for both)",
            (unsigned long long)(pages1 - pages0), (unsigned long long)(pages2 - pages1), (unsigned long long)kBound);
}
