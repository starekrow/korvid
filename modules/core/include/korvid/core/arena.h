// Korvid core: arenas. Linear allocation out of a reserved range committed on demand;
// free is a no-op, reset gives everything back at once, mark/release nest scopes.
// Single owner. The calling thread's scratch arena is the one most code wants.
#pragma once
#include "korvid/core/memory.h"

namespace kv {

struct Arena {
    Allocator a;                 // &a is the Allocator* containers take
    u8*       base      = nullptr;
    usize     reserved  = 0;
    usize     committed = 0;
    usize     used      = 0;
    usize     peak      = 0;
    bool      owns_range = false; // reserved by arena_create; false for arena_from_buffer
};

// Reserves `reserve` bytes of address space; pages are committed as `used` grows.
bool  arena_create(Arena& ar, usize reserve, const char* name, MemTag tag);
// Wraps memory somebody else owns (a static buffer, a real-time block). Never commits.
void  arena_from_buffer(Arena& ar, void* buffer, usize size, const char* name, MemTag tag);
void  arena_destroy(Arena& ar);

void* arena_alloc(Arena& ar, usize size, u32 align = KV_MEM_DEFAULT_ALIGN);
inline usize arena_mark(const Arena& ar) { return ar.used; }
void  arena_release(Arena& ar, usize mark);
void  arena_reset(Arena& ar);

// Value-initialized T in the arena. Plain data only, like everything else here.
template <class T>
T* arena_push(Arena& ar) {
    T* p = (T*)arena_alloc(ar, sizeof(T), alignof(T) > KV_MEM_DEFAULT_ALIGN ? alignof(T) : KV_MEM_DEFAULT_ALIGN);
    if (p) *p = T{};
    return p;
}
template <class T>
T* arena_push_n(Arena& ar, u32 n) {
    return (T*)arena_alloc(ar, sizeof(T) * n, alignof(T) > KV_MEM_DEFAULT_ALIGN ? alignof(T) : KV_MEM_DEFAULT_ALIGN);
}

// The calling thread's scratch arena, created on first use. Frame lifetime on the main
// thread: engine_frame resets it. Use MemScope for anything shorter.
Arena*     mem_scratch_arena();
Allocator* mem_scratch();
void       mem_scratch_reset();

// Mark on construction, release on destruction. For temporaries inside a call.
struct MemScope {
    Arena* arena;
    usize  mark;
    explicit MemScope(Arena* ar = mem_scratch_arena()) : arena(ar), mark(arena_mark(*ar)) {}
    ~MemScope() { arena_release(*arena, mark); }
    MemScope(const MemScope&) = delete;
    MemScope& operator=(const MemScope&) = delete;
};

} // namespace kv
