// The size-class allocator and the heaps: every class hands out aligned, filled blocks
// that free back and pages get reused; a block freed on another thread returns to its
// page's owner; realloc within a class keeps the pointer; guard pages catch nothing in
// normal use and free cleanly; a hard limit fails the allocation that would pass it,
// the commit that would, and names everyone; the engine config puts a module in a heap.
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/job.h"
#include "korvid/core/memory.h"
#include "korvid/core/module.h"
#include "korvid/core/array.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

struct Slots { void** p; u32 per_chunk; u32 chunks; };
void alloc_chunk(u32 chunk, u32, u32, void* user) {
    Slots& s = *(Slots*)user;
    for (u32 k = 0; k < s.per_chunk; ++k) { void* b = mem_alloc(16 + (k % 40) * 8); memset(b, 1, 16); s.p[chunk * s.per_chunk + k] = b; }
}
void free_neighbor(u32 chunk, u32, u32, void* user) {
    Slots& s = *(Slots*)user;
    u32 v = (chunk + 1) % s.chunks;
    for (u32 k = 0; k < s.per_chunk; ++k) { mem_free(s.p[v * s.per_chunk + k]); s.p[v * s.per_chunk + k] = nullptr; }
}

// A module that spawns into a ktable and allocates from its general view, in a limited heap.
struct Blob { u8 bytes[48]; };
constinit const FieldDesc k_blob_fields[] = {KV_FIELDS(Blob, bytes)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Blob, k_blob_fields)};
constinit const KTableDesc k_ktables[] = {{.name = ".blob", .components = "Blob", .capacity = 1u << 20}};
struct LimState { void* block; };
bool lim_startup(Engine&, ModuleInstance& self) {
    LimState* s = arena_push<LimState>(self.arena);
    s->block = alloc_in(&self.general, 1000);
    self.set_state(s);
    return s->block != nullptr;
}
void lim_shutdown(Engine&, ModuleInstance& self) { free_in(&self.general, self.state_as<LimState>()->block, 1000); }
KV_MODULE_DEFINE(k_lim) = {.name = "lim", .components = k_components, .ktables = k_ktables, .startup = lim_startup, .shutdown = lim_shutdown, .memory = {.reserve = 1u << 20}};

constinit const HeapDesc       k_heaps[]  = {{"small", 0, 512 * 1024}};
constinit const ModuleHeapDesc k_assign[] = {{"lim", "small"}};
constinit const ModuleHeapDesc k_bad_mod[] = {{"nope", "small"}};
constinit const ModuleHeapDesc k_bad_heap[] = {{"lim", "nope"}};

} // namespace

void test_heap(kv::TestCtx& c) {
    using namespace kv;
    c.begin("heap");
    MemStats base = mem_stats();
    u64 pages_before = mem_small_pages();

    // Every class: aligned, writable, accounted, freed. Then again: no new pages needed.
    const u32 sizes[] = {1, 16, 17, 48, 100, 128, 200, 256, 500, 1024, 1500, 2048};
    void* blocks[12];
    for (u32 i = 0; i < 12; ++i) { blocks[i] = mem_alloc(sizes[i]); KV_CHECK(c, blocks[i] && ((usize)blocks[i] & 15) == 0); memset(blocks[i], 7, sizes[i]); }
    KV_CHECK(c, mem_stats().live_allocs == base.live_allocs + 12);
    for (u32 i = 0; i < 12; ++i) mem_free(blocks[i]);
    KV_CHECK(c, mem_stats().live_allocs == base.live_allocs);
    u64 pages_after = mem_small_pages();
    for (u32 i = 0; i < 12; ++i) blocks[i] = mem_alloc(sizes[i]);
    KV_CHECK(c, mem_small_pages() == pages_after); // the same pages served again
    for (u32 i = 0; i < 12; ++i) mem_free(blocks[i]);
    KV_CHECK(c, pages_after >= pages_before && mem_small_pages() > 0); // earlier tests may have carved them already

    // A whole page and more of one class, freed in reverse, then forward.
    void* many[3000];
    for (u32 i = 0; i < 3000; ++i) { many[i] = mem_alloc(64); ((u8*)many[i])[63] = (u8)i; }
    for (u32 i = 3000; i-- > 0;) { KV_CHECK(c, ((u8*)many[i])[63] == (u8)i); mem_free(many[i]); }
    for (u32 i = 0; i < 3000; ++i) many[i] = mem_alloc(64);
    for (u32 i = 0; i < 3000; ++i) mem_free(many[i]);
    KV_CHECK(c, mem_stats().live_allocs == base.live_allocs);

    // Realloc within the class keeps the pointer; across classes moves and keeps the bytes.
    u8* r = (u8*)mem_alloc(40);
    memset(r, 5, 40);
    u8* r2 = (u8*)mem_realloc(r, 48);
    KV_CHECK(c, r2 == r && mem_stats().live_bytes == base.live_bytes + 48);
    u8* r3 = (u8*)mem_realloc(r2, 300);
    KV_CHECK(c, r3 != r2 && r3[39] == 5);
    mem_free(r3);

    // Remote frees: blocks allocated by one chunk's thread are freed by another's, and
    // the accounting and the pages come back. Then the owner reuses what others freed.
    jobs_init(3);
    void* slots[16 * 500];
    Slots s{slots, 500, 16};
    jobs_run(16, 1, alloc_chunk, &s);
    jobs_run(16, 1, free_neighbor, &s);
    MemStats with_pool = mem_stats(); // the pool's own thread array and queue are live now
    jobs_run(16, 1, alloc_chunk, &s);
    KV_CHECK(c, mem_stats().live_allocs == with_pool.live_allocs + 8000);
    jobs_run(16, 1, free_neighbor, &s);
    KV_CHECK(c, mem_stats().live_allocs == with_pool.live_allocs);
    jobs_run(16, 1, alloc_chunk, &s);
    jobs_run(16, 1, free_neighbor, &s);
    KV_CHECK(c, mem_stats().live_allocs == with_pool.live_allocs && mem_stats().live_bytes == with_pool.live_bytes);
    jobs_shutdown(); // the workers' pages are orphaned; the next allocations adopt them
    KV_CHECK(c, mem_stats().live_allocs == base.live_allocs);
    u64 pages_now = mem_small_pages();
    for (u32 i = 0; i < 3000; ++i) many[i] = mem_alloc(16);
    for (u32 i = 0; i < 3000; ++i) mem_free(many[i]);
    KV_CHECK(c, mem_small_pages() <= pages_now + 1);

    // Large blocks: malloc, and in dev a guard page for 64 KB and up.
    u8* big = (u8*)mem_alloc(100000);
    KV_CHECK(c, big && ((usize)big & 15) == 0);
    memset(big, 9, 100000);
    KV_CHECK(c, mem_stats().live_bytes == base.live_bytes + 100000);
    mem_free(big);
    u8* mid = (u8*)mem_alloc(5000);
    memset(mid, 3, 5000);
    mem_free(mid);
    KV_CHECK(c, mem_stats().live_allocs == base.live_allocs);

    // A hard limit: blocks past it fail and the count is honest; a commit past it fails.
    u8 heap = mem_heap_register("test.hard", 0, 256 * 1024);
    MemTag tag; tag.heap = heap;
    Allocator view;
    mem_general_tagged(view, tag, "limited");
    void* a1 = alloc_in(&view, 200 * 1024);
    void* a2 = alloc_in(&view, 100 * 1024); // 300 KB: over
    KV_CHECK(c, a1 != nullptr && a2 == nullptr);
    KV_CHECK(c, mem_heap_used(heap) == 200 * 1024);
    void* a3 = alloc_in(&view, 50 * 1024); // 250 KB: fits
    KV_CHECK(c, a3 != nullptr && mem_heap_used(heap) == 250 * 1024);
    free_in(&view, a1, 200 * 1024);
    free_in(&view, a3, 50 * 1024);
    KV_CHECK(c, mem_heap_used(heap) == 0);
    // An array on that heap: growing past the limit fails and leaves it as it was.
    {
        Array<u8> arr = array_in<u8>(&view);
        u8* first = arr.try_push_n(100 * 1024);
        KV_CHECK(c, first != nullptr && arr.len == 100 * 1024);
        memset(first, 7, 100 * 1024);
        u8* before = arr.data;
        u32 cap = arr.cap;
        KV_CHECK(c, arr.try_push_n(200 * 1024) == nullptr && !arr.reserve(300 * 1024)); // would pass 256 KB
        KV_CHECK(c, arr.data == before && arr.cap == cap && arr.len == 100 * 1024 && arr.data[100 * 1024 - 1] == 7);
        KV_CHECK(c, arr.try_push(9) != nullptr && arr.len == 100 * 1024 + 1); // room in what it has
        arr.free();
        KV_CHECK(c, mem_heap_used(heap) == 0);
    }
    Arena ar;
    KV_CHECK(c, arena_create(ar, 4u << 20, "limited", tag));
    KV_CHECK(c, arena_alloc(ar, 100 * 1024) != nullptr);      // commits 128 KB
    KV_CHECK(c, arena_alloc(ar, 1000 * 1024) == nullptr);     // would commit past 256 KB
    KV_CHECK(c, mem_heap_used(heap) == ar.committed);
    arena_destroy(ar);
    KV_CHECK(c, mem_heap_used(heap) == 0);

    // The engine config: the module's general view, arena and ktables carry the heap; a
    // spawn storm hits the hard limit, fails with the message, and the engine goes on.
    EngineConfig cfg;
    cfg.heaps = k_heaps;
    cfg.module_heaps = k_assign;
    const ModuleDesc* const list[] = {&k_lim};
    Engine e;
    e.threads = 1;
    KV_CHECK(c, engine_init(e, list, cfg));
    u8 small = mem_heap_register("small", 0, 0);
    ModuleInstance* lim = engine_module(e, "lim");
    KV_CHECK(c, lim && lim->general.tag.heap == small && lim->arena.a.tag.heap == small);
    KV_CHECK(c, mem_module_heap((u16)(lim->index + 1)) == small);
    KTable* blobs = engine_ktable(e, "lim.blob");
    KV_CHECK(c, blobs->tag.heap == small && strcmp(mem_category_name(blobs->tag.category), "ktables") == 0);
    u64 used_at_start = mem_heap_used(small);
    KV_CHECK(c, used_at_start > 0); // the arena's first commit and the startup block
    u32 spawned = 0;
    for (u32 i = 0; i < 20000; ++i) { if (blobs->spawn(Blob{}).is_null()) break; ++spawned; }
    KV_CHECK(c, spawned > 1000 && spawned < 20000); // 48-byte rows: a few thousand fit in 512 KB
    KV_CHECK(c, mem_heap_used(small) <= 512 * 1024);
    KV_CHECK(c, blobs->live() == spawned && !blobs->spawn(Blob{}).is_null() == false);
    blobs->destroy_all();
    KV_CHECK(c, blobs->live() == 0 && !blobs->spawn(Blob{}).is_null()); // room again: the pages stay committed
    [[maybe_unused]] const u8 blob_category = blobs->tag.category; // the ktable goes with the engine; read in dev
    engine_shutdown(e);
    KV_CHECK(c, mem_heap_used(small) == 0);
    // What is live now includes the process's name table, which the engine's names grew.
    u64 live_after_engine = mem_stats().live_allocs;
#if KV_DEV
    KV_CHECK(c, mem_stats_category(blob_category).committed_bytes == 0);
#endif

    // Bad configs are rejected before any module starts.
    EngineConfig bad1; bad1.heaps = k_heaps; bad1.module_heaps = k_bad_mod;
    EngineConfig bad2; bad2.heaps = k_heaps; bad2.module_heaps = k_bad_heap;
    Engine f;
    KV_CHECK(c, !engine_init(f, list, bad1));
    KV_CHECK(c, !engine_init(f, list, bad2));
    KV_CHECK(c, mem_stats().live_allocs == live_after_engine);
}
