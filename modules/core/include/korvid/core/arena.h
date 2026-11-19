// Korvid core: arenas, and the scratch arena every thread has.
//
// An arena hands out memory front to back from one reserved range and frees all of it at
// once: allocating is a pointer bump, freeing one thing is nothing, and a reset or a
// release to a mark gives back everything after it. That fits memory whose lifetime is a
// phase of something rather than one object: a module's state (self.arena, until the
// module stops), a frame's temporaries (the scratch arena), a parse.
//
// Each arena has one owner thread, checked in dev builds. The calling thread's scratch
// arena (mem_scratch) is the one most code wants, scoped with MemScope:
//
//   MemScope scope;                                  // everything below is freed at '}'
//   JsonDoc doc;
//   if (!json_parse(text, len, mem_scratch(), doc)) return false;
//
// Never keep a pointer into scratch past the frame (on a worker, past the job): the next
// reset reuses it, and dev builds poison it to make that show.
#pragma once
#include "korvid/core/memory.h"

namespace kv {

// A range of address space handed out front to back. Pages commit as `used` grows.
struct Arena {
    Allocator a;                 // &a is the Allocator* containers take
    u8*       base      = nullptr;
    usize     reserved  = 0;
    usize     committed = 0;
    usize     used      = 0;
    usize     peak      = 0;
    bool      owns_range = false; // reserved by arena_create; false for arena_from_buffer
};

// Reserves `reserve` bytes of address space (rounded up to pages), committed as `used`
// grows. False, logged, if the address space can't be had. The calling thread owns it.
// A module doesn't make its own: ModuleDesc::memory gives it self.arena.
bool  arena_create(Arena& ar, usize reserve, const char* name, MemTag tag);
// Wraps memory somebody else owns (a static buffer, a real-time block). Never commits.
void  arena_from_buffer(Arena& ar, void* buffer, usize size, const char* name, MemTag tag);
// Gives the address space back (arena_create's only). Pointers into it are dead.
void  arena_destroy(Arena& ar);

// `size` bytes at `align`, uninitialized (poisoned in dev builds). Null, logged, when the
// reserve is used up or the heap refuses the commit.
void* arena_alloc(Arena& ar, usize size, u32 align = KV_MEM_DEFAULT_ALIGN);
// Where the arena is now, and back to it: everything allocated since the mark is freed
// at once. Marks nest. MemScope does this for a block of code.
inline usize arena_mark(const Arena& ar) { return ar.used; }
void  arena_release(Arena& ar, usize mark);
// Frees everything. Committed pages stay committed for the next use.
void  arena_reset(Arena& ar);

// One value-initialized T in the arena, or null. No destructor will run: plain data, or
// types whose cleanup the owner does by hand (an Array's free in shutdown).
template <class T>
T* arena_push(Arena& ar) {
    T* p = (T*)arena_alloc(ar, sizeof(T), alignof(T) > KV_MEM_DEFAULT_ALIGN ? alignof(T) : KV_MEM_DEFAULT_ALIGN);
    if (p) *p = T{};
    return p;
}
// n uninitialized Ts, or null.
template <class T>
T* arena_push_n(Arena& ar, u32 n) {
    return (T*)arena_alloc(ar, sizeof(T) * n, alignof(T) > KV_MEM_DEFAULT_ALIGN ? alignof(T) : KV_MEM_DEFAULT_ALIGN);
}

// The calling thread's scratch arena, created on first use, for temporaries. On the main
// thread it lives a frame (engine_frame resets it); on a job worker, one job. Use
// MemScope inside it for anything shorter. mem_scratch() is it as an Allocator*, for
// containers and parsers.
Arena*     mem_scratch_arena();
Allocator* mem_scratch();
// Frees the calling thread's scratch: the loop's, once a frame.
void       mem_scratch_reset();
// A thread that is about to end gives its scratch back. Workers call it; main uses mem_shutdown.
void       mem_thread_exit();

// Mark on construction, release on destruction: everything allocated from the arena
// (the calling thread's scratch, by default) while it is in scope is freed when it ends.
//   MemScope scope;
//   Array<u32> cells = array_in<u32>(mem_scratch());   // gone at the closing brace
struct MemScope {
    Arena* arena;
    usize  mark;
    explicit MemScope(Arena* ar = mem_scratch_arena()) : arena(ar), mark(arena_mark(*ar)) {}
    ~MemScope() { arena_release(*arena, mark); }
    MemScope(const MemScope&) = delete;
    MemScope& operator=(const MemScope&) = delete;
};

} // namespace kv
