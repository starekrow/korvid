# Allocators

Nov 6, 2026. Design for the engine's memory system, and its implementation status.

## Principle

**Allocation is ownership.** Every allocator has exactly one owner: a thread, a system,
or the engine. There is no "the heap" that everyone shares. Code written as if it owns
the machine gets an allocator it really does own; sharing is explicit, the same rule the
event bus follows.

- **Lifetime is the primary axis, not size or alignment.** The baked names map to
  lifetimes and owners: general, scratch, frame, arena, pool. Alignment is a parameter
  with a 16-byte default, because it is a property of the data, not of the policy.
- **Flexible underneath, baked on top.** Variants are config instances; engine code sees
  a fixed set of C-like entry points and three types. Containers hold an `Allocator*`,
  never an allocator template parameter.
- **No pointers in sim state, only handles.** Pools are relocatable, snapshots are a
  copy, module unload is one arena release. Dev builds may move a relocatable arena
  between snapshots to catch anyone who kept a pointer.
- **Attribution comes from the schedule, not the call site.** The scheduler sets a
  thread-local current owner before running a system or a module's startup; even a bare
  `mem_alloc` is attributed to the right module with no change at the call site.
- **Budgets are data.** Heap sizes and per-module memory needs are tables, declared the
  way systems and events are.
- **Accounting costs nothing on the allocation path.** Every thread counts into its own
  block; a query rolls the blocks up; `engine_frame` takes one sample per frame that
  cheap readers consult for last frame's totals, per-heap use, budget warnings and the
  sampled peak. The module-by-category matrix exists in dev builds only. When the
  size-class allocator lands, accounting moves to page granularity.
- **The allocator knows no names.** Categories ("textures", "pathfinding") and heaps
  ("sim", "render") are registered by name at startup by whoever wants to see them apart
  in a report. A tag of zeros means nothing and costs nothing; core's own allocators stay
  untagged. The allocator never carries a list of what the engine or a game cares about.

## The family

Five layers, bottom up. Only the bottom one is ported.

**1. Page layer, per platform** (`core/os_memory.h`, `src/<platform>/os_memory.cc`):
reserve, commit, decommit, release, page size. Later: large pages, memory kinds (CPU
cached, write-combined or GPU-visible, console direct memory), cache line size per
platform. Windows is VirtualAlloc, POSIX is mmap plus mprotect/madvise, PS5 is direct
memory plus mapping; Switch has limited virtual address space, so the layers above must
also work with committed chunks where large reservations are unavailable.

**2. Heaps**: registered by name with a budget (`mem_heap_register`), the unit of budget,
accounting and exhaustion policy, carried in every allocation's tag. Heap 0 is the
unbudgeted default. A soft limit warns once; hard limits fail, never a mystery global
OOM. Which heap a module's memory lands in is the root's budgeting decision (pending);
modules declare only what they need.

**3. Allocator kinds.** All implement one fat-pointer interface, no virtuals:

```cpp
struct Allocator {
    void* (*alloc)(Allocator*, usize size, u32 align);
    void  (*free)(Allocator*, void* p, usize size);  // size supplied: no headers needed
    MemTag tag;    // module, category, heap: all zero by default, zero means none
    u32    owner;  // thread id for single-owner kinds; asserted in debug
    MemMode mode;  // SingleOwner, ThreadLocal, Shared
};
```

- **Arena** (linear bump): alloc only, reset at a boundary, mark and release for nested
  scopes. Per-thread scratch, per-job scratch, load-time temporaries, module persistent
  state. Single owner, no locks.
- **Pool** (`Pool<T>`): fixed-size slots with a freelist and generations, handle indexed,
  typed. Storage is one reserved range committed on demand: slots are contiguous,
  addresses stable, a snapshot is one copy. The pool is the allocator for T; it computes
  stride and alignment from T.
- **Size-class small allocator**: 24 classes from 16 bytes to 2 KB, 64 KB pages of one
  class each owned by the allocating thread, a per-thread partial and full list per
  class, a lock-free remote free list per page for alloc-on-A free-on-B; the first remote
  free into a full page pushes the page onto the owner's notify stack, so the owner finds
  pages with room without scanning its full ones (a scan over every full page was a cache
  miss per page and ruined the allocator with millions of blocks live); pages left by an
  exiting thread are adopted by the next thread that needs one, empty pages return to a
  shared stack. Blocks above 2 KB sit
  on malloc for now; the page path above is for consoles.
- **Large allocator**: blocks above about 64 KB go straight to pages. Guard pages in
  debug for free.
- **Frame ring**: N-deep multi-buffered arena reset when the frame fence passes, for
  data handed to the render thread or the GPU. The renderer's deferred-destruction
  queue is the same mechanism keyed by the same fence.
- **Real-time fixed-block**: preallocated, lock-free, no system calls, for the audio
  thread. A thread-local no-alloc flag makes any general allocation there assert.
- **Debug wrapper**: fill on alloc, poison on free, guard pages, owner tracking, leak
  report by tag at shutdown. Absent in ship.

**4. The baked surface** (`core/memory.h`, `core/arena.h`):

- `mem_alloc(size)`, `mem_alloc_aligned(size, align)`, `mem_realloc(p, size)`,
  `mem_free(p)`: the general allocator, thread-safe, 16-byte aligned by default.
- `mem_general()`: the general allocator as an `Allocator*`.
- `mem_scratch()`: the calling thread's scratch arena. Frame lifetime on the main thread
  (reset at the top of `engine_frame`), job lifetime on workers. `MemScope` marks and
  releases it for nested temporaries.
- `Arena`, `Pool<T>`: the types an owner creates for itself.
- `Array<T>` takes an `Allocator*` at creation (`array_in<T>(alloc)`), default general.
- `MemTag`: module id from the registry, a category id, a heap id. Zero by default.
- `mem_category_register(name)`, `mem_heap_register(name, budget)`: idempotent by name,
  startup only. `mem_stats_module/category/heap` and `mem_report` read them back.

**5. Module memory as registration data.** `ModuleDesc::memory` declares the address
space to reserve for the module's arena. `engine_init` creates the module's arena from its heap budget and hands it in
`ModuleInstance::arena`; module state goes there by rule, so unload is one release and
the budget is visible before any code runs.

## Threading rules

- Scratch and small-object caches are thread-local. No locks on the fast path.
- Arenas and pools are single-owner. Debug builds record the owner thread and assert on
  cross-thread use.
- The general allocator is the only implicitly shared allocator.
- Sharing sim state across threads goes through per-thread command buffers merged in a
  fixed order by the owner, like event writers. Spawn and destroy are commands, so a
  pool's freelist is touched only by its owner and slot assignment stays deterministic.
- Real-time threads never touch the general allocator.
- Addresses never affect logic. Handles cross every boundary.

## Game surface (C ABI)

Games get entity and component storage (they allocate entities, not memory);
`kv_mem_alloc`/`kv_mem_free` in the Game heap; `kv_scratch` for call-scoped
temporaries; `kv_arena_*` for their own lifetimes; budget queries. Nothing the engine
returns across the ABI needs freeing by the caller: results are written into
caller-provided spans or arenas, and span types carry their lifetime. The game-side
language runtime's own heap is outside the budget.

## Non-goals

No global `operator new` override. No per-allocation call-site capture outside dev
builds. No allocator template parameters on containers.

## Status

| Step | State |
| --- | --- |
| 1. `Allocator` interface, `MemTag`, schedule-driven attribution, general allocator on malloc with headers (alignment, canary) and per-tag accounting, containers take `Allocator*`, leak report by tag | done |
| 2. Arena, per-thread scratch with `MemScope`, module arenas from `ModuleDesc::memory` | done |
| 3. `Pool<T>` on reserved ranges with stable addresses, snapshot and restore | done |
| 4. Deferred destruction in the renderer keyed by the frame fence; per-frame CPU data in scratch | done (swapchain recreation still waits idle: presentation completion is not fence-tracked) |
| 5. Size-class small allocator: 64 KB pages per class owned by the allocating thread, lock-free remote frees drained by the owner, orphaned pages adopted, empty pages reused | done |
| 6a. Heap hard limits (counted as it happens, admitted before every commit) and root-assigned module heaps (`EngineConfig`), guard pages for 64 KB and up and call-site capture in dev, kind storage attributed under "kinds" | done |
| 6b. Page layers for consoles, frame ring type, real-time allocator | pending |
| Dev checks (`KV_VERIFY`, on in debug and dev): owner thread on arenas and pools, alignment, canary on general blocks, fill 0xCD on alloc, poison 0xDD on free, release, reset and pool destroy | done |
