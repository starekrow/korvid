#include "korvid/core/memory.h"
#include "korvid/core/atomic.h"
#include "korvid/core/log.h"
#include "korvid/core/os_memory.h"
#include "korvid/core/profile.h"
#include "korvid/core/thread.h"
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <dlfcn.h>
#endif

namespace kv {

// ---- spike hunting (memory.h) --------------------------------------------------------------
static bool g_spike_hunt = false; // set before the engine starts; read everywhere after
void dev_spike_hunt(bool on) { g_spike_hunt = on; }
bool dev_spike_hunting() { return g_spike_hunt; }

namespace {

// Every general block starts a header before the pointer handed out. `cls` says where
// the block lives: a size class, the guard-page path, or malloc; for malloc blocks with
// alignments above 16 the block is over-allocated and `offset` says how far back malloc's
// pointer is. The canary is a function of the user pointer, so a header copied or a
// pointer that was never handed out fails the check; a freed block's canary is scrambled
// to catch the second free. Dev builds keep the call site.
struct Header {
    usize  size;   // bytes requested
    u32    offset; // malloc: from the header back to malloc's pointer; guard: from the mapping's base
    u32    align;  // as requested, so realloc can honor it
    MemTag tag;
    u32    canary;
    u8     cls;    // 0..kClasses-1, kClsGuard or kClsMalloc
    u8     pad[7];
#if KV_DEV
    void*  site;   // the caller of mem_alloc
    u64    pad2;
#endif
};
static_assert(sizeof(Header) % 16 == 0, "header keeps 16-byte alignment");
static_assert(sizeof(MemTag) == 4, "tag fits the header");
constexpr u8 kClsGuard  = 0xFE;
constexpr u8 kClsMalloc = 0xFF;

constexpr u32 kCanary = 0x5EEDBA5Eu;
constexpr u8  kFillAlloc = 0xCD;
constexpr u8  kFillFree  = 0xDD;

u32 canary_for(const void* user) {
    usize p = (usize)user;
    return kCanary ^ (u32)(p >> 4) ^ (u32)(p >> 36);
}

struct Counter {
    u64 live_allocs, live_bytes, committed;
};

constexpr u32 kModules = KV_MEM_MAX_MODULES + 1;
#if KV_DEV
Counter g_cells[kModules][KV_MEM_MAX_CATEGORIES];
#endif
thread_local u16 t_owner = 0;

// Names are copied in, truncated to kNameLen - 1 characters, so a caller may build one
// in a scratch buffer and forget it.
constexpr u32 kNameLen = 32;
constexpr u32 kNameSlots = KV_MEM_MAX_CATEGORIES > KV_MEM_MAX_HEAPS ? KV_MEM_MAX_CATEGORIES : KV_MEM_MAX_HEAPS;

struct Registry {
    char  names[kNameSlots][kNameLen];
    usize budgets[KV_MEM_MAX_HEAPS];
    u32   count;
};
Registry g_categories = {{"none"}, {}, 1};
Registry g_heaps_reg  = {{"default"}, {}, 1};
u32      g_heap_warned[KV_MEM_MAX_HEAPS]; // atomic flags
usize    g_heap_hard[KV_MEM_MAX_HEAPS];
u64      g_heap_used[KV_MEM_MAX_HEAPS];   // atomic; bytes against the hard limit, where one exists
u32      g_heap_failed[KV_MEM_MAX_HEAPS]; // atomic flags: the message went out once
u8       g_module_heap[KV_MEM_MAX_MODULES + 1];

u8 registry_add(Registry& r, u32 max, const char* name) {
    if (!name || !name[0]) return 0;
    char key[kNameLen];
    strncpy(key, name, kNameLen - 1);
    key[kNameLen - 1] = 0;
    if (strlen(name) >= kNameLen) KV_WARN("mem", "name '%s' truncated to '%s'", name, key);
    for (u32 i = 0; i < r.count; ++i)
        if (strcmp(r.names[i], key) == 0) return (u8)i;
    if (r.count >= max) {
        KV_WARN("mem", "no room to register '%s'; using the default", key);
        return 0;
    }
    memcpy(r.names[r.count], key, kNameLen);
    return (u8)r.count++;
}

// ---- per-thread accounting ------------------------------------------------------------
// A thread's block is plain memory it alone writes; other threads read it with relaxed
// atomic loads (plain moves on every target). Per-thread values may go negative, since
// a block freed on another thread subtracts there; only the sum means anything. Blocks
// come from a fixed table, are claimed once per thread and never handed back: when a
// thread exits, its totals fold into the retired block and the slot is marked dead.
struct ThreadBlock {
    i64 live_allocs;
    i64 live_bytes;
    i64 total_allocs;
    i64 committed;
    i64 heap_allocs[KV_MEM_MAX_HEAPS];
    i64 heap_bytes[KV_MEM_MAX_HEAPS];
    i64 heap_committed[KV_MEM_MAX_HEAPS];
    u32 in_use;
};
constexpr u32 kMaxThreads = 256;
alignas(64) ThreadBlock g_blocks[kMaxThreads];
ThreadBlock g_retired;      // folded totals of exited threads; written under g_retired_lock
u32         g_retired_lock; // spin, taken once per thread lifetime
u32         g_block_count;  // atomic bump
thread_local ThreadBlock* t_block = nullptr;

inline void bump(i64& v, i64 d) { kv::atomic_store(&v, kv::atomic_load(&v, kv::MO_RELAXED) + d, kv::MO_RELAXED); }
inline i64  peek(const i64& v) { return kv::atomic_load(&v, kv::MO_RELAXED); }

void lock_retired()   { while (kv::atomic_exchange(&g_retired_lock, 1u, kv::MO_ACQUIRE)) {} }
void unlock_retired() { kv::atomic_store(&g_retired_lock, 0u, kv::MO_RELEASE); }

void fold_into_retired(ThreadBlock& b) {
    lock_retired();
    g_retired.live_allocs  += peek(b.live_allocs);
    g_retired.live_bytes   += peek(b.live_bytes);
    g_retired.total_allocs += peek(b.total_allocs);
    g_retired.committed    += peek(b.committed);
    for (u32 h = 0; h < KV_MEM_MAX_HEAPS; ++h) {
        g_retired.heap_allocs[h]    += peek(b.heap_allocs[h]);
        g_retired.heap_bytes[h]     += peek(b.heap_bytes[h]);
        g_retired.heap_committed[h] += peek(b.heap_committed[h]);
    }
    unlock_retired();
}

// Folds the thread's block when the thread goes away.
struct ThreadBlockGuard {
    ~ThreadBlockGuard() {
        if (!t_block) return;
        fold_into_retired(*t_block);
        *t_block = ThreadBlock{};
        kv::atomic_store(&t_block->in_use, 0u, kv::MO_RELEASE);
        t_block = nullptr;
    }
};
thread_local ThreadBlockGuard t_guard;

ThreadBlock& block() {
    if (!t_block) {
        u32 i = kv::atomic_fetch_add(&g_block_count, 1u, kv::MO_ACQ_REL);
        if (i >= kMaxThreads) {
            kv::atomic_fetch_sub(&g_block_count, 1u, kv::MO_ACQ_REL);
            // Past the table: this thread counts into a private block that rollups do
            // not see until the thread exits and the guard folds it. Rare; 256 threads.
            static thread_local ThreadBlock overflow;
            t_block = &overflow;
            return overflow;
        }
        t_block = &g_blocks[i];
        kv::atomic_store(&t_block->in_use, 1u, kv::MO_RELEASE);
        (void)t_guard; // odr-use: the guard exists for this thread
    }
    return *t_block;
}

void account_alloc(MemTag tag, i64 bytes) {
    ThreadBlock& b = block();
    bump(b.live_allocs, 1);
    bump(b.live_bytes, bytes);
    bump(b.total_allocs, 1);
    bump(b.heap_allocs[tag.heap], 1);
    bump(b.heap_bytes[tag.heap], bytes);
#if KV_DEV
    Counter& c = g_cells[tag.module][tag.category];
    kv::atomic_add_fetch(&c.live_allocs, 1, kv::MO_RELAXED);
    kv::atomic_add_fetch(&c.live_bytes, (u64)bytes, kv::MO_RELAXED);
#endif
}

void account_free(MemTag tag, i64 bytes) {
    ThreadBlock& b = block();
    bump(b.live_allocs, -1);
    bump(b.live_bytes, -bytes);
    bump(b.heap_allocs[tag.heap], -1);
    bump(b.heap_bytes[tag.heap], -bytes);
#if KV_DEV
    Counter& c = g_cells[tag.module][tag.category];
    kv::atomic_sub_fetch(&c.live_allocs, 1, kv::MO_RELAXED);
    kv::atomic_sub_fetch(&c.live_bytes, (u64)bytes, kv::MO_RELAXED);
#endif
}

// Sums every live block and the retired totals.
ThreadBlock rollup() {
    ThreadBlock sum{};
    lock_retired();
    sum = g_retired;
    unlock_retired();
    u32 n = kv::atomic_load(&g_block_count, kv::MO_ACQUIRE);
    if (n > kMaxThreads) n = kMaxThreads;
    for (u32 i = 0; i < n; ++i) {
        const ThreadBlock& b = g_blocks[i];
        if (!kv::atomic_load(&b.in_use, kv::MO_ACQUIRE)) continue;
        sum.live_allocs  += peek(b.live_allocs);
        sum.live_bytes   += peek(b.live_bytes);
        sum.total_allocs += peek(b.total_allocs);
        sum.committed    += peek(b.committed);
        for (u32 h = 0; h < KV_MEM_MAX_HEAPS; ++h) {
            sum.heap_allocs[h]    += peek(b.heap_allocs[h]);
            sum.heap_bytes[h]     += peek(b.heap_bytes[h]);
            sum.heap_committed[h] += peek(b.heap_committed[h]);
        }
    }
    return sum;
}

u64 nonneg(i64 v) { return v > 0 ? (u64)v : 0; }

MemSample g_sample;

// ---- dev-only matrix ----------------------------------------------------------------
#if KV_DEV
MemStats stats_of(const Counter& c) {
    MemStats s;
    s.live_allocs     = kv::atomic_load(&c.live_allocs, kv::MO_RELAXED);
    s.live_bytes      = kv::atomic_load(&c.live_bytes, kv::MO_RELAXED);
    s.committed_bytes = kv::atomic_load(&c.committed, kv::MO_RELAXED);
    return s;
}
void accumulate(MemStats& s, const Counter& c) {
    MemStats t = stats_of(c);
    s.live_allocs += t.live_allocs; s.live_bytes += t.live_bytes; s.committed_bytes += t.committed_bytes;
}
#endif

// Out-of-range tags are a bug upstream (engine_init bounds the module count); dev builds
// say so, ship folds them into the defaults rather than indexing out of bounds.
MemTag clamp(MemTag t) {
    KV_VERIFY(t.module <= KV_MEM_MAX_MODULES && t.category < KV_MEM_MAX_CATEGORIES && t.heap < KV_MEM_MAX_HEAPS);
    if (t.module > KV_MEM_MAX_MODULES) t.module = 0;
    if (t.category >= KV_MEM_MAX_CATEGORIES) t.category = 0;
    if (t.heap >= KV_MEM_MAX_HEAPS) t.heap = 0;
    return t;
}

const char* module_name_of(u16 module);

// ---- heap hard limits ------------------------------------------------------------------
bool heap_admit(MemTag tag, usize bytes) {
    u8 h = tag.heap;
    if (!h || !g_heap_hard[h]) return true;
    u64 now = kv::atomic_add_fetch(&g_heap_used[h], (u64)bytes, kv::MO_RELAXED);
    if (now <= g_heap_hard[h]) return true;
    kv::atomic_sub_fetch(&g_heap_used[h], (u64)bytes, kv::MO_RELAXED);
    KV_MARK_V("heap refused", bytes);
    if (kv::atomic_exchange(&g_heap_failed[h], 1u, kv::MO_RELAXED) == 0)
        KV_ERROR("mem", "heap '%s' hard limit %llu bytes: module '%s' category '%s' asked for %llu more with %llu in use",
                 g_heaps_reg.names[h], (unsigned long long)g_heap_hard[h], module_name_of(tag.module),
                 g_categories.names[tag.category], (unsigned long long)bytes, (unsigned long long)(now - bytes));
    return false;
}

void heap_release(MemTag tag, usize bytes) {
    u8 h = tag.heap;
    if (h && g_heap_hard[h]) kv::atomic_sub_fetch(&g_heap_used[h], (u64)bytes, kv::MO_RELAXED);
}

// ---- the size-class allocator ------------------------------------------------------------
// Pages of 64 KB, 64 KB aligned, carved from one reserved region, each holding blocks of
// one class. A page belongs to the thread that carved (or adopted) it: only the owner
// pops blocks and pushes its own frees; everyone else pushes onto the page's remote
// list with a CAS, and the owner drains that list when its own runs dry.
constexpr u32   kPageShift = 16;
constexpr usize kPageSize  = (usize)1 << kPageShift;
constexpr usize kRegion    = (usize)4 << 30; // address space, not memory
constexpr u32   kClasses   = 24;
constexpr u16   kClassSize[kClasses] = {16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512,
                                        640, 768, 896, 1024, 1280, 1536, 1792, 2048};
constexpr usize kSmallMax  = 2048;
constexpr u32   kPageHeader = 128;
constexpr u32   kPageMagic  = 0x70616765u; // "page"

struct Notify;
struct Page {
    u32     magic;
    u32     owner;       // thread id; 0 while free or orphaned
    u32     stride;      // header plus class size, a multiple of 16
    u32     capacity;
    u32     fresh;       // blocks never handed out yet start here
    u32     used;        // atomic: live blocks
    u8      cls;
    u8      in_full;     // on the owner's full list rather than its partial list
    u8      pad[2];
    u32     nstate;      // atomic: where the page stands with its owner's notify stack (below)
    void*   local_free;  // owner only
    void*   remote_free; // atomic stack, pushed by other threads
    Page*   next;        // owner's list, or the free or orphan stack
    Page*   prev;        // owner's list
    Notify* notify;      // atomic: the owner's notify stacks; null while orphaned
    Page*   notify_next; // link in a notify stack
};
static_assert(sizeof(Page) <= kPageHeader, "the page header fits before the first block");

// Where other threads tell an owner that a full page of its has blocks again: the first
// remote free into an empty remote list pushes the page here. One per thread, kept for
// the life of the process so a push racing a thread's exit lands somewhere harmless; a
// thread's stacks are read by that thread alone, and by no one once it exits.
struct Notify {
    Page*   head[kClasses]; // atomic stacks
    Notify* all;            // every Notify ever made, for shutdown
};

// A page is on at most one notify stack, once. It has one link, so a second push while it
// is still on the stack rewrites the link and cuts off everything below it, whose pages
// then keep their freed blocks out of their owner's reach for good. `nstate` holds which of
// these the page is in (low bits) and a generation (the rest), bumped by every push:
//   idle      on no stack; the next remote free that finds the remote list empty pushes it
//   pushing   a remote freer has claimed the push and is making it
//   queued    on its owner's stack; the owner sets it idle as it pops it
//   adopting  an orphan being taken by a new owner; no push starts
// A pusher's last step moves pushing to queued only if the generation is still its own, so
// a pop that came first leaves the page idle, and a late pusher never takes a later push's
// state for its own.
constexpr u32 kNotifyIdle     = 0;
constexpr u32 kNotifyPushing  = 1;
constexpr u32 kNotifyQueued   = 2;
constexpr u32 kNotifyAdopting = 3;
constexpr u32 kNotifyMask     = 3;
constexpr u32 kNotifyGen      = 4;

inline u32 nstate_with(u32 s, u32 state) { return (s & ~kNotifyMask) | state; }
Notify* g_notifies = nullptr; // under g_page_lock

u8*   g_region       = nullptr;
u32   g_next_page    = 0;      // atomic bump
u64   g_pages_carved = 0;      // atomic, for reports
Page* g_free_pages   = nullptr; // under g_page_lock
Page* g_orphans[kClasses];      // under g_page_lock: pages whose owner thread exited
u32   g_page_lock    = 0;

void lock_pages()   { while (kv::atomic_exchange(&g_page_lock, 1u, kv::MO_ACQUIRE)) {} }
void unlock_pages() { kv::atomic_store(&g_page_lock, 0u, kv::MO_RELEASE); }

u8 class_of(usize size) {
    for (u8 c = 0; c < kClasses; ++c) if (size <= kClassSize[c]) return c;
    return kClsMalloc;
}

u8*   block_at(Page* p, u32 i) { return (u8*)p + kPageHeader + (usize)i * p->stride; }
Page* page_of(const void* user) { return (Page*)((usize)user & ~(kPageSize - 1)); }

// Per thread: a partial list (pages with room, the head is where allocation starts) and
// a full list, per class. Pages are given up when the thread exits.
struct Cache {
    Page*   partial[kClasses];
    Page*   full[kClasses];
    Notify* notify;
};
thread_local Cache t_cache;

void list_push(Page*& head, Page* p) { p->prev = nullptr; p->next = head; if (head) head->prev = p; head = p; }
void list_remove(Page*& head, Page* p) {
    if (p->prev) p->prev->next = p->next; else head = p->next;
    if (p->next) p->next->prev = p->prev;
    p->next = p->prev = nullptr;
}

Notify* my_notify() {
    Cache& c = t_cache;
    if (!c.notify) {
        Notify* n = (Notify*)calloc(1, sizeof(Notify));
        lock_pages();
        n->all = g_notifies;
        g_notifies = n;
        unlock_pages();
        c.notify = n;
    }
    return c.notify;
}

// A fresh page (zeroed by the OS), or one from the free pool, which is idle and which no
// remote freer can reach (it has no live blocks; see small_free). nstate keeps its
// generation.
void page_init(Page* p, u8 cls) {
    u32 stride = (u32)((sizeof(Header) + kClassSize[cls] + 15) & ~15u);
    p->magic = kPageMagic; p->owner = thread_id(); p->stride = stride;
    p->capacity = (u32)((kPageSize - kPageHeader) / stride);
    p->fresh = 0; p->used = 0; p->cls = cls; p->in_full = 0;
    p->local_free = p->remote_free = nullptr;
    p->next = p->prev = nullptr;
    p->notify_next = nullptr;
    kv::atomic_store(&p->notify, my_notify(), kv::MO_RELEASE);
}

// An orphan for this thread. Its notify state is still its dead owner's: queued on a stack
// no one will read again, or being pushed there by a free that began before the owner
// exited. Wait out such a push (a few instructions on another thread), close the page to
// new ones, point it at this thread's stacks, and open it again. Its link on the dead stack
// is abandoned, which is safe because no one walks that stack again.
void page_adopt(Page* p) {
    u32 s = kv::atomic_load(&p->nstate, kv::MO_ACQUIRE);
    for (;;) {
        if ((s & kNotifyMask) == kNotifyPushing) { s = kv::atomic_load(&p->nstate, kv::MO_ACQUIRE); continue; }
        if (kv::atomic_compare_exchange(&p->nstate, &s, nstate_with(s, kNotifyAdopting), false, kv::MO_ACQ_REL, kv::MO_ACQUIRE)) break;
    }
    p->notify_next = nullptr;
    kv::atomic_store(&p->notify, my_notify(), kv::MO_RELEASE);
    kv::atomic_store(&p->nstate, nstate_with(s, kNotifyIdle), kv::MO_RELEASE);
}

struct CacheGuard {
    ~CacheGuard() {
        // Whatever the thread still owns waits for another thread to adopt it. A free that
        // reads the page's notify after this pushes nowhere; one that read it before pushes
        // onto this thread's stacks, which page_adopt then abandons.
        lock_pages();
        for (u32 c = 0; c < kClasses; ++c) {
            Page* lists[2] = {t_cache.partial[c], t_cache.full[c]};
            for (Page* p : lists)
                while (p) {
                    Page* n = p->next;
                    p->owner = 0;
                    kv::atomic_store(&p->notify, (Notify*)nullptr, kv::MO_RELEASE);
                    p->prev = nullptr; p->next = g_orphans[c]; g_orphans[c] = p;
                    p = n;
                }
            t_cache.partial[c] = t_cache.full[c] = nullptr;
        }
        unlock_pages();
        // The stacks are no one's now. A destructor that allocates after this one gets new
        // ones rather than popping pages that other threads are adopting.
        t_cache.notify = nullptr;
    }
};
thread_local CacheGuard t_cache_guard;

// A page for class `cls`, owned by this thread: an orphan, a freed page, or a fresh one.
Page* page_new(u8 cls) {
    (void)t_cache_guard; // odr-use: the guard exists for this thread
    lock_pages();
    Page* p = g_orphans[cls];
    if (p) {
        g_orphans[cls] = p->next;
        p->owner = thread_id(); p->next = p->prev = nullptr; p->in_full = 0;
        unlock_pages();
        page_adopt(p); // outside the lock: it may wait on a push in flight
        return p;
    }
    p = g_free_pages;
    if (p) g_free_pages = p->next;
    unlock_pages();
    if (!p) {
        if (!g_region) {
            lock_pages();
            if (!g_region) {
                u8* raw = (u8*)os_reserve(kRegion + kPageSize);
                if (raw) g_region = (u8*)(((usize)raw + kPageSize - 1) & ~(kPageSize - 1));
            }
            unlock_pages();
            if (!g_region) return nullptr;
        }
        u32 i = kv::atomic_fetch_add(&g_next_page, 1u, kv::MO_RELAXED);
        if ((usize)i * kPageSize >= kRegion) { KV_ERROR("mem", "the small allocator's region is exhausted"); return nullptr; }
        p = (Page*)(g_region + (usize)i * kPageSize);
        if (!os_commit(p, kPageSize)) { KV_ERROR("mem", "small allocator: commit failed"); return nullptr; }
        kv::atomic_add_fetch(&g_pages_carved, 1, kv::MO_RELAXED);
    }
    page_init(p, cls);
    return p;
}

// Takes the remote list in one exchange and makes it the owner's.
void drain_remote(Page* p) {
    void* r = kv::atomic_exchange(&p->remote_free, (void*)nullptr, kv::MO_ACQ_REL);
    if (!r) return;
    if (!p->local_free) { p->local_free = r; return; }
    void* tail = r;
    while (*(void**)tail) tail = *(void**)tail;
    *(void**)tail = p->local_free;
    p->local_free = r;
}

// A block from this page, or null when it has none left anywhere.
u8* page_take(Page* p) {
    if (!p->local_free) {
        if (p->fresh < p->capacity) return block_at(p, p->fresh++);
        drain_remote(p);
        if (!p->local_free) return nullptr;
    }
    u8* b = (u8*)p->local_free;
    p->local_free = *(void**)b;
    return b;
}

// Full pages that other threads have freed into since the owner last looked: the notify
// stack, taken whole. Only pages this thread still owns and still has as full count. Each
// page's link is read before the page is marked idle: from then on a remote free may push
// it again and rewrite the link.
bool collect_notified(Cache& c, u8 cls) {
    Notify* n = c.notify;
    if (!n || !kv::atomic_load(&n->head[cls], kv::MO_ACQUIRE)) return false;
    Page* p = kv::atomic_exchange(&n->head[cls], (Page*)nullptr, kv::MO_ACQ_REL);
    bool found = false;
    while (p) {
        Page* next = p->notify_next;
        p->notify_next = nullptr;
        // Queued, or still pushing if its pusher has not marked it yet (that pusher's last
        // step then finds the page idle and leaves it so). Nothing else is possible here.
        u32 s = kv::atomic_load(&p->nstate, kv::MO_RELAXED);
        kv::atomic_store(&p->nstate, nstate_with(s, kNotifyIdle), kv::MO_RELEASE);
        if (p->owner == thread_id() && p->in_full) {
            drain_remote(p);
            if (p->local_free) { list_remove(c.full[cls], p); list_push(c.partial[cls], p); p->in_full = 0; found = true; }
        }
        p = next;
    }
    return found;
}

u8* small_take(u8 cls) {
    Cache& c = t_cache;
    for (;;) {
        Page* p = c.partial[cls];
        while (p) {
            if (u8* b = page_take(p)) return b;
            Page* n = p->next;
            list_remove(c.partial[cls], p); // no room: to the full list until a free shows
            list_push(c.full[cls], p);
            p->in_full = 1;
            p = n;
        }
        if (collect_notified(c, cls)) continue;
        Page* np = page_new(cls);
        if (!np) return nullptr;
        list_push(c.partial[cls], np);
    }
}

// Pushes the page onto its owner's notify stack, unless it is on one already, is being
// pushed, or is changing hands; the page's state says which, and only the remote free that
// moves it from idle to pushing makes the push. The caller holds a block of the page that
// `used` still counts, so the page is not pooled under it.
void notify_owner(Page* p) {
    u32 s = kv::atomic_load(&p->nstate, kv::MO_RELAXED);
    if ((s & kNotifyMask) != kNotifyIdle) return; // queued, pushing or adopting: it will be seen
    u32 pushing = nstate_with(s + kNotifyGen, kNotifyPushing);
    if (!kv::atomic_compare_exchange(&p->nstate, &s, pushing, false, kv::MO_ACQUIRE, kv::MO_RELAXED)) return; // another free won
    Notify* n = kv::atomic_load(&p->notify, kv::MO_ACQUIRE);
    if (n) {
        Page* top = kv::atomic_load(&n->head[p->cls], kv::MO_RELAXED);
        do { p->notify_next = top; } while (!kv::atomic_compare_exchange(&n->head[p->cls], &top, p, true, kv::MO_RELEASE, kv::MO_RELAXED));
    }
    // Queued, if the owner has not popped it already (then it is idle, and stays so); with
    // no owner (orphaned as this ran), idle again for whoever adopts it.
    u32 expect = pushing;
    kv::atomic_compare_exchange(&p->nstate, &expect, nstate_with(pushing, n ? kNotifyQueued : kNotifyIdle), false,
                                kv::MO_RELEASE, kv::MO_RELAXED);
}

void small_free(void* user) {
    Page* p = page_of(user);
    u8* b = (u8*)user - sizeof(Header);
    if (p->owner == thread_id()) {
        *(void**)b = p->local_free;
        p->local_free = b;
        u32 left = kv::atomic_sub_fetch(&p->used, 1u, kv::MO_ACQ_REL);
        Cache& c = t_cache;
        // An empty page that is not where allocation starts goes back to the pool; a full
        // page with a block again is partial again. Not one still queued on this thread's
        // notify stack: the pool would hand it to a thread that re-links it while it is
        // still on the stack. It is collected in time and pooled when next empty. At zero
        // every remote freer has finished (each drops `used` last), so the state is settled.
        if (left == 0 && c.partial[p->cls] != p &&
            (kv::atomic_load(&p->nstate, kv::MO_ACQUIRE) & kNotifyMask) == kNotifyIdle) {
            if (p->in_full) list_remove(c.full[p->cls], p); else list_remove(c.partial[p->cls], p);
            lock_pages();
            p->owner = 0; p->notify = nullptr; p->next = g_free_pages; g_free_pages = p;
            unlock_pages();
        } else if (p->in_full) {
            list_remove(c.full[p->cls], p);
            list_push(c.partial[p->cls], p);
            p->in_full = 0;
        }
        return;
    }
    void* head = kv::atomic_load(&p->remote_free, kv::MO_RELAXED);
    do { *(void**)b = head; } while (!kv::atomic_compare_exchange(&p->remote_free, &head, (void*)b, true, kv::MO_ACQ_REL, kv::MO_RELAXED));
    // First into an empty remote list: tell the owner, which may have nothing else telling
    // it this page has blocks again. Before `used` drops: while it counts this block the
    // page cannot be pooled and re-initialized under the push (its class, its notify).
    if (head == nullptr) notify_owner(p);
    kv::atomic_sub_fetch(&p->used, 1u, kv::MO_ACQ_REL);
}

// ---- the general entry points ------------------------------------------------------------
void fill_header(Header* h, usize size, u32 align, MemTag tag, u8 cls, u32 offset, void* site) {
    void* user = (u8*)h + sizeof(Header);
    h->size = size; h->offset = offset; h->align = align; h->tag = tag; h->canary = canary_for(user); h->cls = cls;
#if KV_DEV
    h->site = site;
    if (!dev_spike_hunting()) memset(user, kFillAlloc, size);
#else
    (void)site;
#endif
}

void* general_alloc_tagged(usize size, u32 align, MemTag tag, void* site) {
    KV_VERIFY(!thread_is_realtime());
    KV_VERIFY(align >= 1 && align <= KV_MEM_MAX_ALIGN && (align & (align - 1)) == 0);
    if (align < KV_MEM_DEFAULT_ALIGN) align = KV_MEM_DEFAULT_ALIGN;
    if (align > KV_MEM_MAX_ALIGN) align = KV_MEM_MAX_ALIGN;
    tag = clamp(tag);
    usize slack = align > KV_MEM_DEFAULT_ALIGN ? align : 0;
    if (size > SIZE_MAX - sizeof(Header) - slack - kPageSize) {
        KV_ERROR("mem", "allocation size overflows: %llu bytes", (unsigned long long)size);
        return nullptr;
    }
    if (!heap_admit(tag, size)) return nullptr;

    if (align == KV_MEM_DEFAULT_ALIGN && size <= kSmallMax) {
        u8 cls = class_of(size);
        u8* b = small_take(cls);
        if (!b) { heap_release(tag, size); KV_ERROR("mem", "out of memory: %llu bytes", (unsigned long long)size); return nullptr; }
        Page* p = page_of(b);
        kv::atomic_add_fetch(&p->used, 1u, kv::MO_RELAXED);
        Header* h = (Header*)b;
        fill_header(h, size, align, tag, cls, 0, site);
        account_alloc(tag, (i64)size);
        return b + sizeof(Header);
    }
#if KV_DEV
    if (size >= kPageSize && align <= os_page_size() && !dev_spike_hunting()) {
        // Its own pages, the block flush against a guard page: one byte past the end faults.
        usize ps = os_page_size();
        usize data = (size + sizeof(Header) + ps - 1) & ~(ps - 1);
        u8* base = (u8*)os_reserve(data + ps);
        if (!base || !os_commit(base, data)) { heap_release(tag, size); KV_ERROR("mem", "out of memory: %llu bytes", (unsigned long long)size); return nullptr; }
        usize user = ((usize)base + data - size) & ~((usize)align - 1);
        Header* h = (Header*)(user - sizeof(Header));
        fill_header(h, size, align, tag, kClsGuard, (u32)((u8*)h - base), site);
        account_alloc(tag, (i64)size);
        return (void*)user;
    }
#endif
    usize total = sizeof(Header) + size + slack;
    u8* raw = (u8*)malloc(total);
    if (!raw) { heap_release(tag, size); KV_ERROR("mem", "out of memory: %llu bytes", (unsigned long long)size); return nullptr; }
    usize user = ((usize)raw + sizeof(Header) + (align - 1)) & ~((usize)align - 1);
    Header* h = (Header*)(user - sizeof(Header));
    fill_header(h, size, align, tag, kClsMalloc, (u32)((u8*)h - raw), site);
    account_alloc(tag, (i64)size);
    return (void*)user;
}

// The header behind a pointer the general allocator handed out, or null with a message
// if it is not one (or not any more).
Header* header_of(void* p) {
    Header* h = (Header*)((u8*)p - sizeof(Header));
    if (h->canary != canary_for(p)) {
        KV_ERROR("mem", "%p is not a live general allocation (bad canary): wrong allocator or double free", p);
        KV_VERIFY(!"bad canary");
        return nullptr;
    }
    return h;
}

MemTag owner_tag() {
    MemTag t;
    t.module = t_owner;
    t.heap   = g_module_heap[t_owner];
    return t;
}

extern Allocator g_general;

// The default instance attributes to the current owner; a tagged view attributes to its
// tag as written, module 0 included (that is how engine-owned shared tables are tagged).
void* general_alloc(Allocator* a, usize size, u32 align) {
    MemTag tag = a->tag;
    if (a == &g_general) tag = owner_tag();
    return general_alloc_tagged(size, align, tag, KV_RETURN_ADDRESS());
}
void general_free(Allocator*, void* p, usize) { mem_free(p); }

Allocator g_general = {general_alloc, general_free, MemTag{}, 0, MemMode::Shared, "general"};

} // namespace

void* mem_alloc(usize size) { return general_alloc_tagged(size, KV_MEM_DEFAULT_ALIGN, owner_tag(), KV_RETURN_ADDRESS()); }
void* mem_alloc_aligned(usize size, u32 align) { return general_alloc_tagged(size, align, owner_tag(), KV_RETURN_ADDRESS()); }

void* mem_realloc(void* p, usize size) {
    if (!p) return mem_alloc(size);
    Header* h = header_of(p);
    if (!h) return nullptr;
    if (h->cls < kClasses && size <= kClassSize[h->cls] && size > (h->cls ? kClassSize[h->cls - 1] : 0)) {
        // Same class: the block stays where it is.
        MemTag tag = clamp(h->tag);
        if (size > h->size && !heap_admit(tag, size - h->size)) return nullptr;
        if (size < h->size) heap_release(tag, h->size - size);
        account_free(tag, (i64)h->size);
        account_alloc(tag, (i64)size);
        h->size = size;
        return p;
    }
    void* q = general_alloc_tagged(size, h->align, h->tag, KV_RETURN_ADDRESS()); // same alignment, same tag
    if (!q) return nullptr;
    memcpy(q, p, h->size < size ? h->size : size);
    mem_free(p);
    return q;
}

void mem_free(void* p) {
    if (!p) return;
    Header* h = header_of(p);
    if (!h) return; // never free what we did not allocate
    MemTag tag = clamp(h->tag);
    account_free(tag, (i64)h->size);
    heap_release(tag, h->size);
    h->canary = ~h->canary; // a second free fails the canary
#if KV_DEV
    if (!dev_spike_hunting()) memset(p, kFillFree, h->size);
#endif
    if (h->cls < kClasses) { small_free(p); return; }
    if (h->cls == kClsGuard) {
        usize ps = os_page_size();
        u8* base = (u8*)h - h->offset;
        usize data = (h->size + sizeof(Header) + ps - 1) & ~(ps - 1);
        os_release(base, data + ps);
        return;
    }
    free((u8*)h - h->offset);
}

Allocator* mem_general() { return &g_general; }

void mem_general_tagged(Allocator& out, MemTag tag, const char* name) {
    out      = g_general;
    out.tag  = tag;
    out.name = name;
}

u64 mem_small_pages() { return kv::atomic_load(&g_pages_carved, kv::MO_RELAXED); }

void mem_set_owner(u16 module) { t_owner = module; }
u16  mem_owner() { return t_owner; }
void mem_set_module_heap(u16 module, u8 heap) { if (module <= KV_MEM_MAX_MODULES && heap < KV_MEM_MAX_HEAPS) g_module_heap[module] = heap; }
u8   mem_module_heap(u16 module) { return module <= KV_MEM_MAX_MODULES ? g_module_heap[module] : 0; }

u8          mem_category_register(const char* name) { return registry_add(g_categories, KV_MEM_MAX_CATEGORIES, name); }
const char* mem_category_name(u8 id) { return id < g_categories.count ? g_categories.names[id] : "?"; }

u8 mem_heap_register(const char* name, usize soft, usize hard) {
    u8 id = registry_add(g_heaps_reg, KV_MEM_MAX_HEAPS, name);
    if (id && soft) g_heaps_reg.budgets[id] = soft;
    if (id && hard) g_heap_hard[id] = hard;
    return id;
}
const char* mem_heap_name(u8 id) { return id < g_heaps_reg.count ? g_heaps_reg.names[id] : "?"; }
usize       mem_heap_budget(u8 id) { return id < KV_MEM_MAX_HEAPS ? g_heaps_reg.budgets[id] : 0; }
usize       mem_heap_hard(u8 id) { return id < KV_MEM_MAX_HEAPS ? g_heap_hard[id] : 0; }
u64         mem_heap_used(u8 id) { return id < KV_MEM_MAX_HEAPS ? kv::atomic_load(&g_heap_used[id], kv::MO_RELAXED) : 0; }
bool        mem_heap_admit(MemTag tag, usize bytes) { return heap_admit(clamp(tag), bytes); }

u64 mem_live_allocations() { return nonneg(rollup().live_allocs); }

MemStats mem_stats() {
    ThreadBlock t = rollup();
    MemStats s;
    s.live_allocs     = nonneg(t.live_allocs);
    s.live_bytes      = nonneg(t.live_bytes);
    s.total_allocs    = nonneg(t.total_allocs);
    s.committed_bytes = nonneg(t.committed);
    return s;
}

MemStats mem_stats_heap(u8 heap) {
    MemStats s;
    if (heap >= KV_MEM_MAX_HEAPS) return s;
    ThreadBlock t = rollup();
    s.live_allocs     = nonneg(t.heap_allocs[heap]);
    s.live_bytes      = nonneg(t.heap_bytes[heap]);
    s.committed_bytes = nonneg(t.heap_committed[heap]);
    return s;
}

MemStats mem_stats_module(u16 module) {
    MemStats s;
#if KV_DEV
    if (module <= KV_MEM_MAX_MODULES)
        for (u32 c = 0; c < KV_MEM_MAX_CATEGORIES; ++c) accumulate(s, g_cells[module][c]);
#else
    (void)module;
#endif
    return s;
}

MemStats mem_stats_category(u8 category) {
    MemStats s;
#if KV_DEV
    if (category < KV_MEM_MAX_CATEGORIES)
        for (u32 m = 0; m < kModules; ++m) accumulate(s, g_cells[m][category]);
#else
    (void)category;
#endif
    return s;
}

void mem_account_commit(MemTag tag, usize bytes) {
    tag = clamp(tag);
    ThreadBlock& b = block();
    bump(b.committed, (i64)bytes);
    bump(b.heap_committed[tag.heap], (i64)bytes);
#if KV_DEV
    kv::atomic_add_fetch(&g_cells[tag.module][tag.category].committed, bytes, kv::MO_RELAXED);
#endif
}

void mem_account_decommit(MemTag tag, usize bytes) {
    tag = clamp(tag);
    heap_release(tag, bytes);
    ThreadBlock& b = block();
    bump(b.committed, -(i64)bytes);
    bump(b.heap_committed[tag.heap], -(i64)bytes);
#if KV_DEV
    kv::atomic_sub_fetch(&g_cells[tag.module][tag.category].committed, bytes, kv::MO_RELAXED);
#endif
}

void mem_sample(u64 frame) {
    ThreadBlock t = rollup();
    MemSample& s = g_sample;
    s.frame           = frame;
    s.live_allocs     = nonneg(t.live_allocs);
    s.live_bytes      = nonneg(t.live_bytes);
    s.committed_bytes = nonneg(t.committed);
    if (s.live_bytes > s.peak_live_bytes) s.peak_live_bytes = s.live_bytes;
    for (u32 h = 0; h < KV_MEM_MAX_HEAPS; ++h) {
        s.heap_committed[h] = nonneg(t.heap_committed[h]);
        s.heap_bytes[h]     = nonneg(t.heap_bytes[h]) + s.heap_committed[h];
        usize budget = g_heaps_reg.budgets[h];
        if (budget && s.heap_bytes[h] > budget && kv::atomic_exchange(&g_heap_warned[h], 1u, kv::MO_RELAXED) == 0)
            KV_WARN("mem", "heap '%s' over budget: %llu of %llu bytes", g_heaps_reg.names[h],
                    (unsigned long long)s.heap_bytes[h], (unsigned long long)budget);
    }
}

const MemSample& mem_last_sample() { return g_sample; }

static const char* g_module_names[KV_MEM_MAX_MODULES];
static u32         g_module_name_count = 0;

namespace {
const char* module_name_of(u16 module) {
    if (module == 0) return "engine";
    return (u32)(module - 1) < g_module_name_count && g_module_names[module - 1] ? g_module_names[module - 1] : "?";
}
} // namespace

void mem_set_module_names(const char* const* module_names, u32 module_count) {
    if (module_count > KV_MEM_MAX_MODULES) module_count = KV_MEM_MAX_MODULES;
    for (u32 i = 0; i < module_count; ++i) g_module_names[i] = module_names[i];
    g_module_name_count = module_count;
}

const char* const* mem_module_names() { return g_module_names; }
u32                mem_module_name_count() { return g_module_name_count; }

void mem_report(const char* const* module_names, u32 module_count) {
    MemStats total = mem_stats();
    KV_INFO("mem", "live: %llu allocations, %llu bytes, %llu committed; sampled peak %llu bytes",
            (unsigned long long)total.live_allocs, (unsigned long long)total.live_bytes,
            (unsigned long long)total.committed_bytes, (unsigned long long)g_sample.peak_live_bytes);
#if KV_DEV
    for (u32 m = 0; m < kModules; ++m) {
        for (u32 c = 0; c < g_categories.count; ++c) {
            MemStats s = stats_of(g_cells[m][c]);
            if (!s.live_allocs && !s.committed_bytes) continue;
            const char* who = m == 0 ? "engine" : (m - 1 < module_count ? module_names[m - 1] : "?");
            KV_WARN("mem", "live: %s/%s: %llu allocations, %llu bytes, %llu committed", who,
                    g_categories.names[c], (unsigned long long)s.live_allocs,
                    (unsigned long long)s.live_bytes, (unsigned long long)s.committed_bytes);
        }
    }
    // The first few live small blocks, with where they came from: the pages can be walked.
    u32 listed = 0;
    u32 pages = kv::atomic_load(&g_next_page, kv::MO_RELAXED);
    for (u32 i = 0; i < pages && listed < 8 && g_region; ++i) {
        Page* p = (Page*)(g_region + (usize)i * kPageSize);
        if (p->magic != kPageMagic || kv::atomic_load(&p->used, kv::MO_RELAXED) == 0) continue;
        for (u32 k = 0; k < p->fresh && listed < 8; ++k) {
            Header* h = (Header*)block_at(p, k);
            void* user = (u8*)h + sizeof(Header);
            if (h->canary != canary_for(user)) continue;
            const char* who = h->tag.module == 0 ? "engine" : ((u32)(h->tag.module - 1) < module_count ? module_names[h->tag.module - 1] : "?");
            // The site as an offset into its binary, which is what addr2line wants from a
            // position-independent executable.
            void* base = nullptr;
            const char* image = "<exe>";
#if defined(__linux__)
            Dl_info info;
            if (h->site && dladdr(h->site, &info)) { base = info.dli_fbase; image = info.dli_fname; }
#endif
            KV_WARN("mem", "  %llu bytes by %s/%s from %p (addr2line -e %s 0x%llx)", (unsigned long long)h->size, who,
                    g_categories.names[h->tag.category], h->site, image, (unsigned long long)((usize)h->site - (usize)base));
            ++listed;
        }
    }
#else
    (void)module_names; (void)module_count;
#endif
    KV_INFO("mem", "small allocator: %llu pages of %llu KB carved", (unsigned long long)mem_small_pages(), (unsigned long long)(kPageSize / 1024));
}

} // namespace kv
