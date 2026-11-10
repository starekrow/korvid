#include "korvid/core/arena.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_memory.h"
#include "korvid/core/thread.h"
#include <string.h>

namespace kv {

namespace {

constexpr usize kCommitChunk    = 64 * 1024;
constexpr usize kScratchReserve = 256u * 1024 * 1024; // address space, not memory
constexpr u8    kFillReleased   = 0xDD;

// Dev builds poison released space, so anything that kept a pointer across a release
// (the arena-plus-scope hazard) reads 0xDD instead of stale data that happens to work.
void poison(Arena& ar, usize from, usize to) {
#if KV_DEV
    if (to > ar.committed) to = ar.committed;
    if (to > from && !dev_spike_hunting()) memset(ar.base + from, kFillReleased, to - from);
#else
    (void)ar; (void)from; (void)to;
#endif
}

void* arena_alloc_fn(Allocator* a, usize size, u32 align) {
    return arena_alloc(*(Arena*)a, size, align); // Allocator is the first member
}
void arena_free_fn(Allocator*, void*, usize) {} // arenas free at reset

bool commit_to(Arena& ar, usize needed) {
    if (needed <= ar.committed) return true;
    KV_VERIFY(!thread_is_realtime()); // committing is a system call
    if (!ar.owns_range) {
        KV_ERROR("mem", "arena '%s' is full: %llu of %llu bytes", ar.a.name,
                 (unsigned long long)needed, (unsigned long long)ar.reserved);
        return false;
    }
    if (needed > ar.reserved) {
        KV_ERROR("mem", "arena '%s' exceeds its reservation: %llu of %llu bytes", ar.a.name,
                 (unsigned long long)needed, (unsigned long long)ar.reserved);
        return false;
    }
    usize grow = ar.committed ? ar.committed : kCommitChunk;
    while (ar.committed + grow < needed) grow *= 2;
    if (ar.committed + grow > ar.reserved) grow = ar.reserved - ar.committed;
    grow = os_page_align(grow);
    if (!mem_heap_admit(ar.a.tag, grow)) return false; // the hard limit said so, with the message
    if (!os_commit(ar.base + ar.committed, grow)) {
        KV_ERROR("mem", "arena '%s': commit of %llu bytes failed", ar.a.name, (unsigned long long)grow);
        return false;
    }
    ar.committed += grow;
    mem_account_commit(ar.a.tag, grow);
    return true;
}

} // namespace

bool arena_create(Arena& ar, usize reserve, const char* name, MemTag tag) {
    ar = Arena{};
    ar.a.alloc = arena_alloc_fn;
    ar.a.free  = arena_free_fn;
    ar.a.tag   = tag;
    ar.a.owner = thread_id();
    ar.a.mode  = MemMode::SingleOwner;
    ar.a.name  = name;
    reserve = os_page_align(reserve);
    ar.base = (u8*)os_reserve(reserve);
    if (!ar.base) {
        KV_ERROR("mem", "arena '%s': cannot reserve %llu bytes", name, (unsigned long long)reserve);
        return false;
    }
    ar.reserved   = reserve;
    ar.owns_range = true;
    return true;
}

void arena_from_buffer(Arena& ar, void* buffer, usize size, const char* name, MemTag tag) {
    ar = Arena{};
    ar.a.alloc = arena_alloc_fn;
    ar.a.free  = arena_free_fn;
    ar.a.tag   = tag;
    ar.a.owner = thread_id();
    ar.a.mode  = MemMode::SingleOwner;
    ar.a.name  = name;
    ar.base      = (u8*)buffer;
    ar.reserved  = size;
    ar.committed = size;
}

void arena_destroy(Arena& ar) {
    if (ar.owns_range && ar.base) {
        mem_account_decommit(ar.a.tag, ar.committed);
        os_release(ar.base, ar.reserved);
    }
    ar = Arena{};
}

void* arena_alloc(Arena& ar, usize size, u32 align) {
    KV_VERIFY(ar.a.mode != MemMode::SingleOwner || ar.a.owner == thread_id());
    KV_VERIFY(align >= 1 && align <= KV_MEM_MAX_ALIGN && (align & (align - 1)) == 0);
    usize start = (ar.used + (align - 1)) & ~((usize)align - 1);
    if (size > ar.reserved || start > ar.reserved - size) { // would wrap or overrun
        KV_ERROR("mem", "arena '%s': %llu bytes do not fit", ar.a.name, (unsigned long long)size);
        return nullptr;
    }
    usize end = start + size;
    if (!commit_to(ar, end)) return nullptr;
    ar.used = end;
    if (end > ar.peak) ar.peak = end;
    return ar.base + start;
}

void arena_release(Arena& ar, usize mark) {
    KV_VERIFY(ar.a.mode != MemMode::SingleOwner || ar.a.owner == thread_id());
    KV_VERIFY(mark <= ar.used);
    if (mark > ar.used) return;
    poison(ar, mark, ar.used);
    ar.used = mark;
}

void arena_reset(Arena& ar) {
    poison(ar, 0, ar.used);
    ar.used = 0;
}

// ---- scratch -------------------------------------------------------------------------

static constinit thread_local Arena t_scratch;
static constinit thread_local bool  t_scratch_ready = false;

Arena* mem_scratch_arena() {
    if (!t_scratch_ready) {
        if (!arena_create(t_scratch, kScratchReserve, "scratch", MemTag{})) return nullptr;
        t_scratch.a.mode = MemMode::ThreadLocal;
        t_scratch_ready  = true;
    }
    return &t_scratch;
}

Allocator* mem_scratch() { return &mem_scratch_arena()->a; }

void mem_scratch_reset() {
    if (t_scratch_ready) arena_reset(t_scratch);
}

void mem_thread_exit() {
    if (t_scratch_ready) {
        arena_destroy(t_scratch);
        t_scratch_ready = false;
    }
}

void mem_shutdown() {
    mem_thread_exit();
    MemStats s = mem_stats();
    if (s.live_allocs || s.committed_bytes) mem_report(mem_module_names(), mem_module_name_count());
}

} // namespace kv
