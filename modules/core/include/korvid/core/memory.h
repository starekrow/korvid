// Korvid core: memory: the general allocator, the Allocator interface every container
// takes, and the accounting that says where the bytes went.
//
// Every allocation has an owner, and the engine always knows whose code is running, so
// memory is charged to the module that asked for it: mem_report and the leak report at
// exit name the module and category, and a heap's limit applies to the modules the
// executable put in it (EngineConfig::heaps). Which allocator to use:
//
//   what lives as long as the module      self.arena (arena_push<T>), in startup
//   what grows: arrays, maps              self.general, set as the container's `alloc`;
//                                         free it in shutdown
//   temporaries inside a call or a frame  mem_scratch() under a MemScope (arena.h)
//   entities                              ktables: the engine allocates them
//   anything else                         mem_alloc / mem_free, charged to the running module
//
// Nothing here throws: an allocation that can't be had returns null, logged once.
// Arenas and pools are in arena.h and pool.h; docs/design/allocators.md has the design.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// Who an allocation is charged to. All zero by default, and zero means nothing: no
// registration, no lookup.
//   module:   the registry's module index + 1 (0: the engine itself)
//   category: a name somebody registered with mem_category_register (0: none)
//   heap:     a budget boundary registered with mem_heap_register (0: the default heap)
// The allocator does not know what categories or heaps mean; whoever cares names them.
struct MemTag {
    u16 module   = 0;
    u8  category = 0;
    u8  heap     = 0;
};

// Which threads may use an allocator; checked in debug builds.
enum class MemMode : u8 {
    SingleOwner, // one thread, recorded at creation, asserted in debug
    ThreadLocal, // one per thread by construction, never shared
    Shared,      // safe from any thread
};

// The one interface containers see: two functions and a tag, not a virtual class. Every
// arena, pool view and general-allocator view is one, so an Array or a JSON parse takes an
// Allocator* and doesn't care which. Frees pass the size, so arenas and pools need no
// per-block header.
struct Allocator {
    void* (*alloc)(Allocator* a, usize size, u32 align) = nullptr;
    void  (*free)(Allocator* a, void* p, usize size)    = nullptr; // size as allocated; pools and arenas need no header
    MemTag      tag;
    u32         owner = 0; // thread id for SingleOwner
    MemMode     mode  = MemMode::Shared;
    const char* name  = "";
};

constexpr u32 KV_MEM_DEFAULT_ALIGN = 16;    // SIMD-safe without thinking
constexpr u32 KV_MEM_MAX_ALIGN     = 65536; // anything larger is a design error, not a request

// Calling an Allocator without spelling it twice: bytes, or typed items.
inline void* alloc_in(Allocator* a, usize size, u32 align = KV_MEM_DEFAULT_ALIGN) { return a->alloc(a, size, align); }
inline void  free_in(Allocator* a, void* p, usize size) { if (p) a->free(a, p, size); }
template <class T>
T* alloc_items(Allocator* a, u32 count = 1) {
    return (T*)a->alloc(a, sizeof(T) * count, alignof(T) > KV_MEM_DEFAULT_ALIGN ? alignof(T) : KV_MEM_DEFAULT_ALIGN);
}
template <class T>
void free_items(Allocator* a, T* p, u32 count = 1) { if (p) a->free(a, p, sizeof(T) * count); }

// ---- the general allocator -----------------------------------------------------------
// Thread-safe. Blocks carry a header (size, alignment, tag, canary; the call site in dev)
// so mem_free needs no size, realloc keeps the alignment, and a bad or double free is
// caught at the canary. Blocks up to 2 KB come from size-class pages of 64 KB owned by
// the allocating thread: a free on the owner is a push on the page's list, a free from
// anywhere else is a lock-free push on the page's remote list that the owner drains when
// it needs a block; a thread that exits leaves its pages for others to adopt. Larger
// blocks sit on malloc; in dev builds, blocks of 64 KB and up sit on their own pages
// with a guard page after them. Dev builds fill new blocks with 0xCD and freed ones
// with 0xDD.
//
// malloc and free, the engine's: null on failure (out of memory, or past a heap's hard
// limit, logged). Charged to whichever module's code is running. mem_free(null) is fine.
void*      mem_alloc(usize size);
void*      mem_alloc_aligned(usize size, u32 align);
void*      mem_realloc(void* p, usize size);
void       mem_free(void* p);
// The general allocator as an Allocator*, untagged: what a container with no alloc uses.
Allocator* mem_general();
// A view of the general allocator with a fixed tag, for containers a module owns:
// their growth is then attributed to the owner, whichever system triggers it. The tag
// is used as written; module 0 in a view means the engine itself.
void       mem_general_tagged(Allocator& out, MemTag tag, const char* name);

// ---- internal: attribution ----------------------------------------------------------
// The scheduler sets the current owner (module index + 1) around a module's startup,
// shutdown and each system run; allocations made meanwhile are tagged with it, and with
// the heap the engine assigned to that module.
void mem_set_owner(u16 module);
u16  mem_owner();
void mem_set_module_heap(u16 module, u8 heap);
u8   mem_module_heap(u16 module);

// ---- categories and heaps -------------------------------------------------------------
// Categories and heaps are registered by name, at startup, by whoever cares to see them
// apart in reports. Registering a name twice returns the same id. The name is copied
// (31 characters kept), so it may come from any buffer. Not thread-safe: register during
// startup, use anywhere. Returns 0 (the default) when the table is full or the name is empty.
constexpr u32 KV_MEM_MAX_MODULES    = 64;  // ids 1..64; 0 is the engine
constexpr u32 KV_MEM_MAX_CATEGORIES = 64;  // ids 1..63; 0 is "none"
constexpr u32 KV_MEM_MAX_HEAPS      = 16;  // ids 1..15; 0 is the default heap

u8          mem_category_register(const char* name);
const char* mem_category_name(u8 id);
// A heap past its soft limit warns once, at the frame sample. A heap with a hard limit
// counts every general block and committed page as it happens, and an allocation that
// would pass it fails, with the heap, module and category named, once per heap. 0: none.
u8          mem_heap_register(const char* name, usize soft, usize hard = 0);
const char* mem_heap_name(u8 id);
usize       mem_heap_budget(u8 id); // the soft limit
usize       mem_heap_hard(u8 id);
u64         mem_heap_used(u8 id);   // bytes counted against the hard limit; 0 for heaps without one
// The precheck for a commit: true, and counted, if the heap admits `bytes` more; false,
// with the message, if that would pass the hard limit. Arenas, pools and tables ask
// before committing; mem_account_decommit gives the bytes back.
bool        mem_heap_admit(MemTag tag, usize bytes);

// ---- accounting ----------------------------------------------------------------------
// No atomics on the allocation path: every thread counts into its own block, and a
// query rolls the blocks up. Per-heap and total counts exist in every build; the
// module-by-category matrix exists in dev builds only (shared relaxed atomics there,
// zeros in ship). Peaks are sampled, not tracked: see mem_sample below.
struct MemStats {
    u64 live_allocs     = 0; // general allocator blocks
    u64 live_bytes      = 0; // general allocator bytes
    u64 total_allocs    = 0;
    u64 committed_bytes = 0; // pages committed by arenas and pools
};

u64      mem_live_allocations();           // rollup
MemStats mem_stats();                      // rollup, everything
MemStats mem_stats_heap(u8 heap);          // rollup
MemStats mem_stats_module(u16 module);     // dev builds only
MemStats mem_stats_category(u8 category);  // dev builds only
// Logs what is live per module and category (dev); a leak report when called at shutdown.
void     mem_report(const char* const* module_names, u32 module_count);
// Names for the report, by module index; the engine sets them, the pointers must stay
// valid (module descriptors are constinit, so theirs do).
void     mem_set_module_names(const char* const* module_names, u32 module_count);
const char* const* mem_module_names();
u32                mem_module_name_count();

// Once per frame, engine_frame rolls everything up, checks heap budgets and records the
// result. Anyone wanting a fast read of memory pressure uses the last sample: plain
// loads, no rollup. peak_live_bytes is the highest sampled total this run.
struct MemSample {
    u64 frame           = 0;
    u64 live_allocs     = 0;
    u64 live_bytes      = 0;
    u64 committed_bytes = 0;
    u64 peak_live_bytes = 0;
    u64 heap_bytes[KV_MEM_MAX_HEAPS]     = {}; // general + committed, per heap
    u64 heap_committed[KV_MEM_MAX_HEAPS] = {};
};
void             mem_sample(u64 frame);
const MemSample& mem_last_sample();

// ---- internal: hooks for the other allocators -----------------------------------------
// Accounting hooks for the other allocator kinds (arena, pool): pages committed, after
// mem_heap_admit said yes; decommit also releases the heap's count.
void mem_account_commit(MemTag tag, usize bytes);
void mem_account_decommit(MemTag tag, usize bytes);
// The size-class allocator's pages: how many 64 KB pages it has carved, for reports.
u64  mem_small_pages();

// ---- shutdown -------------------------------------------------------------------------
// Frees the calling thread's scratch, then reports anything still live: the last call
// of the process, after engine_shutdown and names_shutdown. A leak is reported with its
// module, category and, in dev builds, its call site.
void mem_shutdown();

// ---- spike hunting --------------------------------------------------------------------
// Dev builds do work that is not checking: they fill memory as it is handed out and given
// back, poison what is destroyed (a ktable's row, a pool's item, an arena's released range),
// give each large allocation pages of its own behind a guard page, and note every event's
// emitter for the trace. It catches stale pointers and overruns; it also writes megabytes
// nobody reads and maps pages per allocation, so a dev build's timings carry spikes a ship
// build has not got. dev_spike_hunt(true) (the frame loop's --spike-hunt) stops all of it
// and keeps the rest of a dev build: asserts, declared-access checks, the counters, the
// profiler, the logs. Set it before the engine starts. Ship builds do none of it anyway.
void dev_spike_hunt(bool on);
bool dev_spike_hunting();

} // namespace kv
