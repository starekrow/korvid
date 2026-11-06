#include "korvid/core/memory.h"
#include "korvid/core/log.h"
#include "korvid/core/thread.h"
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

// Every general block starts 32 bytes before the pointer handed out. For alignments
// above 16 the block is over-allocated and `offset` says how far back malloc's pointer is.
// The canary is a function of the user pointer, so a header copied or a pointer that was
// never handed out fails the check; a freed block's canary is scrambled to catch the
// second free.
struct Header {
    usize  size;   // bytes requested
    u32    offset; // from the header back to malloc's pointer
    u32    align;  // as requested, so realloc can honor it
    MemTag tag;
    u32    canary;
    u64    reserved; // call site, when dev builds capture one
};
static_assert(sizeof(Header) == 32, "header keeps 16-byte alignment");
static_assert(sizeof(MemTag) == 4, "tag fits the header");

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

inline void bump(i64& v, i64 d) { __atomic_store_n(&v, __atomic_load_n(&v, __ATOMIC_RELAXED) + d, __ATOMIC_RELAXED); }
inline i64  peek(const i64& v) { return __atomic_load_n(&v, __ATOMIC_RELAXED); }

void lock_retired()   { while (__atomic_exchange_n(&g_retired_lock, 1u, __ATOMIC_ACQUIRE)) {} }
void unlock_retired() { __atomic_store_n(&g_retired_lock, 0u, __ATOMIC_RELEASE); }

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
        __atomic_store_n(&t_block->in_use, 0u, __ATOMIC_RELEASE);
        t_block = nullptr;
    }
};
thread_local ThreadBlockGuard t_guard;

ThreadBlock& block() {
    if (!t_block) {
        u32 i = __atomic_fetch_add(&g_block_count, 1u, __ATOMIC_ACQ_REL);
        if (i >= kMaxThreads) {
            __atomic_fetch_sub(&g_block_count, 1u, __ATOMIC_ACQ_REL);
            // Past the table: this thread counts into a private block that rollups do
            // not see until the thread exits and the guard folds it. Rare; 256 threads.
            static thread_local ThreadBlock overflow;
            t_block = &overflow;
            return overflow;
        }
        t_block = &g_blocks[i];
        __atomic_store_n(&t_block->in_use, 1u, __ATOMIC_RELEASE);
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
    __atomic_add_fetch(&c.live_allocs, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&c.live_bytes, (u64)bytes, __ATOMIC_RELAXED);
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
    __atomic_sub_fetch(&c.live_allocs, 1, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&c.live_bytes, (u64)bytes, __ATOMIC_RELAXED);
#endif
}

// Sums every live block and the retired totals.
ThreadBlock rollup() {
    ThreadBlock sum{};
    lock_retired();
    sum = g_retired;
    unlock_retired();
    u32 n = __atomic_load_n(&g_block_count, __ATOMIC_ACQUIRE);
    if (n > kMaxThreads) n = kMaxThreads;
    for (u32 i = 0; i < n; ++i) {
        const ThreadBlock& b = g_blocks[i];
        if (!__atomic_load_n(&b.in_use, __ATOMIC_ACQUIRE)) continue;
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
    s.live_allocs     = __atomic_load_n(&c.live_allocs, __ATOMIC_RELAXED);
    s.live_bytes      = __atomic_load_n(&c.live_bytes, __ATOMIC_RELAXED);
    s.committed_bytes = __atomic_load_n(&c.committed, __ATOMIC_RELAXED);
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

void* general_alloc_tagged(usize size, u32 align, MemTag tag) {
    KV_VERIFY(!thread_is_realtime());
    KV_VERIFY(align >= 1 && align <= KV_MEM_MAX_ALIGN && (align & (align - 1)) == 0);
    if (align < KV_MEM_DEFAULT_ALIGN) align = KV_MEM_DEFAULT_ALIGN;
    if (align > KV_MEM_MAX_ALIGN) align = KV_MEM_MAX_ALIGN;
    // Room for the header, then enough slack to align the user pointer. Refuse a size
    // that would wrap instead of handing back a block smaller than asked.
    usize slack = align > KV_MEM_DEFAULT_ALIGN ? align : 0;
    if (size > SIZE_MAX - sizeof(Header) - slack) {
        KV_ERROR("mem", "allocation size overflows: %llu bytes", (unsigned long long)size);
        return nullptr;
    }
    usize total = sizeof(Header) + size + slack;
    u8* raw = (u8*)malloc(total);
    if (!raw) { KV_ERROR("mem", "out of memory: %llu bytes", (unsigned long long)size); return nullptr; }
    usize user = ((usize)raw + sizeof(Header) + (align - 1)) & ~((usize)align - 1);
    Header* h = (Header*)(user - sizeof(Header));
    tag        = clamp(tag);
    h->size    = size;
    h->offset  = (u32)((u8*)h - raw);
    h->align   = align;
    h->tag     = tag;
    h->canary  = canary_for((void*)user);
    h->reserved = 0;
#if KV_DEV
    memset((void*)user, kFillAlloc, size);
#endif
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
    return t;
}

// The default instance attributes to the current owner; a tagged view attributes to its tag.
void* general_alloc(Allocator* a, usize size, u32 align) {
    MemTag tag = a->tag;
    if (tag.module == 0) tag.module = t_owner;
    return general_alloc_tagged(size, align, tag);
}
void general_free(Allocator*, void* p, usize) { mem_free(p); }

Allocator g_general = {general_alloc, general_free, MemTag{}, 0, MemMode::Shared, "general"};

} // namespace

void* mem_alloc(usize size) { return general_alloc_tagged(size, KV_MEM_DEFAULT_ALIGN, owner_tag()); }
void* mem_alloc_aligned(usize size, u32 align) { return general_alloc_tagged(size, align, owner_tag()); }

void* mem_realloc(void* p, usize size) {
    if (!p) return mem_alloc(size);
    Header* h = header_of(p);
    if (!h) return nullptr;
    void* q = general_alloc_tagged(size, h->align, h->tag); // same alignment, same tag
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
    h->canary = ~h->canary; // a second free fails the canary
#if KV_DEV
    memset(p, kFillFree, h->size);
#endif
    free((u8*)h - h->offset);
}

Allocator* mem_general() { return &g_general; }

void mem_general_tagged(Allocator& out, MemTag tag, const char* name) {
    out      = g_general;
    out.tag  = tag;
    out.name = name;
}

void mem_set_owner(u16 module) { t_owner = module; }
u16  mem_owner() { return t_owner; }

u8          mem_category_register(const char* name) { return registry_add(g_categories, KV_MEM_MAX_CATEGORIES, name); }
const char* mem_category_name(u8 id) { return id < g_categories.count ? g_categories.names[id] : "?"; }

u8 mem_heap_register(const char* name, usize budget) {
    u8 id = registry_add(g_heaps_reg, KV_MEM_MAX_HEAPS, name);
    if (id && budget) g_heaps_reg.budgets[id] = budget;
    return id;
}
const char* mem_heap_name(u8 id) { return id < g_heaps_reg.count ? g_heaps_reg.names[id] : "?"; }
usize       mem_heap_budget(u8 id) { return id < KV_MEM_MAX_HEAPS ? g_heaps_reg.budgets[id] : 0; }

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
    __atomic_add_fetch(&g_cells[tag.module][tag.category].committed, bytes, __ATOMIC_RELAXED);
#endif
}

void mem_account_decommit(MemTag tag, usize bytes) {
    tag = clamp(tag);
    ThreadBlock& b = block();
    bump(b.committed, -(i64)bytes);
    bump(b.heap_committed[tag.heap], -(i64)bytes);
#if KV_DEV
    __atomic_sub_fetch(&g_cells[tag.module][tag.category].committed, bytes, __ATOMIC_RELAXED);
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
        if (budget && s.heap_bytes[h] > budget && __atomic_exchange_n(&g_heap_warned[h], 1u, __ATOMIC_RELAXED) == 0)
            KV_WARN("mem", "heap '%s' over budget: %llu of %llu bytes", g_heaps_reg.names[h],
                    (unsigned long long)s.heap_bytes[h], (unsigned long long)budget);
    }
}

const MemSample& mem_last_sample() { return g_sample; }

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
#else
    (void)module_names; (void)module_count;
#endif
}

} // namespace kv
