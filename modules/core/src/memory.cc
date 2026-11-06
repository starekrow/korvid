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
    u64 live_allocs, live_bytes, peak_bytes, total_allocs, committed;
};

constexpr u32 kModules = KV_MEM_MAX_MODULES + 1;

Counter g_cells[kModules][KV_MEM_MAX_CATEGORIES];
Counter g_heaps[KV_MEM_MAX_HEAPS];
Counter g_total; // exact global peak
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

void add(Counter& c, u64 bytes) {
    __atomic_add_fetch(&c.live_allocs, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&c.total_allocs, 1, __ATOMIC_RELAXED);
    u64 live = __atomic_add_fetch(&c.live_bytes, bytes, __ATOMIC_RELAXED);
    u64 peak = __atomic_load_n(&c.peak_bytes, __ATOMIC_RELAXED);
    while (live > peak && !__atomic_compare_exchange_n(&c.peak_bytes, &peak, live, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

void sub(Counter& c, u64 bytes) {
    __atomic_sub_fetch(&c.live_allocs, 1, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&c.live_bytes, bytes, __ATOMIC_RELAXED);
}

MemStats stats_of(const Counter& c) {
    MemStats s;
    s.live_allocs     = __atomic_load_n(&c.live_allocs, __ATOMIC_RELAXED);
    s.live_bytes      = __atomic_load_n(&c.live_bytes, __ATOMIC_RELAXED);
    s.peak_bytes      = __atomic_load_n(&c.peak_bytes, __ATOMIC_RELAXED);
    s.total_allocs    = __atomic_load_n(&c.total_allocs, __ATOMIC_RELAXED);
    s.committed_bytes = __atomic_load_n(&c.committed, __ATOMIC_RELAXED);
    return s;
}

void accumulate(MemStats& s, const Counter& c) {
    MemStats t = stats_of(c);
    s.live_allocs += t.live_allocs; s.live_bytes += t.live_bytes; s.peak_bytes += t.peak_bytes;
    s.total_allocs += t.total_allocs; s.committed_bytes += t.committed_bytes;
}

// Out-of-range tags are a bug upstream (engine_init bounds the module count); dev builds
// say so, ship folds them into the defaults rather than indexing out of bounds.
MemTag clamp(MemTag t) {
    KV_VERIFY(t.module <= KV_MEM_MAX_MODULES && t.category < KV_MEM_MAX_CATEGORIES && t.heap < KV_MEM_MAX_HEAPS);
    if (t.module > KV_MEM_MAX_MODULES) t.module = 0;
    if (t.category >= KV_MEM_MAX_CATEGORIES) t.category = 0;
    if (t.heap >= KV_MEM_MAX_HEAPS) t.heap = 0;
    return t;
}

Counter& cell(MemTag t) { return g_cells[t.module][t.category]; }

void check_budget(u8 heap) {
    usize budget = g_heaps_reg.budgets[heap];
    if (!budget || __atomic_load_n(&g_heap_warned[heap], __ATOMIC_RELAXED)) return;
    Counter& h = g_heaps[heap];
    u64 used = __atomic_load_n(&h.live_bytes, __ATOMIC_RELAXED) + __atomic_load_n(&h.committed, __ATOMIC_RELAXED);
    if (used > budget && __atomic_exchange_n(&g_heap_warned[heap], 1u, __ATOMIC_RELAXED) == 0) {
        KV_WARN("mem", "heap '%s' over budget: %llu of %llu bytes", g_heaps_reg.names[heap],
                (unsigned long long)used, (unsigned long long)budget);
    }
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
    add(cell(tag), size);
    add(g_heaps[tag.heap], size);
    add(g_total, size);
    check_budget(tag.heap);
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
    sub(cell(tag), h->size);
    sub(g_heaps[tag.heap], h->size);
    sub(g_total, h->size);
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

u64 mem_live_allocations() { return __atomic_load_n(&g_total.live_allocs, __ATOMIC_RELAXED); }

MemStats mem_stats() {
    MemStats s = stats_of(g_total);
    for (u32 m = 0; m < kModules; ++m)
        for (u32 c = 0; c < KV_MEM_MAX_CATEGORIES; ++c)
            s.committed_bytes += __atomic_load_n(&g_cells[m][c].committed, __ATOMIC_RELAXED);
    return s;
}

MemStats mem_stats_module(u16 module) {
    MemStats s;
    if (module <= KV_MEM_MAX_MODULES)
        for (u32 c = 0; c < KV_MEM_MAX_CATEGORIES; ++c) accumulate(s, g_cells[module][c]);
    return s;
}

MemStats mem_stats_category(u8 category) {
    MemStats s;
    if (category < KV_MEM_MAX_CATEGORIES)
        for (u32 m = 0; m < kModules; ++m) accumulate(s, g_cells[m][category]);
    return s;
}

MemStats mem_stats_heap(u8 heap) {
    MemStats s;
    if (heap < KV_MEM_MAX_HEAPS) accumulate(s, g_heaps[heap]);
    return s;
}

void mem_account_commit(MemTag tag, usize bytes) {
    tag = clamp(tag);
    __atomic_add_fetch(&cell(tag).committed, bytes, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_heaps[tag.heap].committed, bytes, __ATOMIC_RELAXED);
    check_budget(tag.heap);
}

void mem_account_decommit(MemTag tag, usize bytes) {
    tag = clamp(tag);
    __atomic_sub_fetch(&cell(tag).committed, bytes, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&g_heaps[tag.heap].committed, bytes, __ATOMIC_RELAXED);
}

void mem_report(const char* const* module_names, u32 module_count) {
    bool any = false;
    for (u32 m = 0; m < kModules; ++m) {
        for (u32 c = 0; c < g_categories.count; ++c) {
            MemStats s = stats_of(g_cells[m][c]);
            if (!s.live_allocs && !s.committed_bytes) continue;
            const char* who = m == 0 ? "engine" : (m - 1 < module_count ? module_names[m - 1] : "?");
            KV_WARN("mem", "live: %s/%s: %llu allocations, %llu bytes (peak %llu), %llu committed", who,
                    g_categories.names[c], (unsigned long long)s.live_allocs,
                    (unsigned long long)s.live_bytes, (unsigned long long)s.peak_bytes,
                    (unsigned long long)s.committed_bytes);
            any = true;
        }
    }
    if (!any) KV_INFO("mem", "nothing live");
}

} // namespace kv
