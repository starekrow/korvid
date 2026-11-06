#include "korvid/core/arena.h"
#include "korvid/core/array.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_memory.h"
#include "korvid/core/test.h"
#include <string.h>

void test_memory(kv::TestCtx& c) {
    using namespace kv;
    c.begin("memory");

    MemStats before = mem_stats();

    // Categories and heaps are names somebody registers; registering twice is the same id,
    // and nobody has to register anything to use the allocator.
    u8 cat_a = mem_category_register("test.a");
    u8 cat_b = mem_category_register("test.b");
    KV_CHECK(c, cat_a != 0 && cat_b != 0 && cat_a != cat_b);
    KV_CHECK(c, mem_category_register("test.a") == cat_a);
    KV_CHECK(c, strcmp(mem_category_name(cat_b), "test.b") == 0 && strcmp(mem_category_name(0), "none") == 0);
    {
        char scratch_name[16];
        memcpy(scratch_name, "test.c", 7);
        u8 cat_c = mem_category_register(scratch_name);
        memset(scratch_name, 'x', sizeof scratch_name); // the registry copied it
        KV_CHECK(c, cat_c != 0 && strcmp(mem_category_name(cat_c), "test.c") == 0);
        KV_CHECK(c, mem_category_register("test.c") == cat_c);
    }
    KV_CHECK(c, mem_category_register("") == 0 && mem_category_register(nullptr) == 0);
    u8 heap = mem_heap_register("test.heap", 1u << 20);
    KV_CHECK(c, heap != 0 && mem_heap_register("test.heap", 0) == heap && mem_heap_budget(heap) == 1u << 20);
    KV_CHECK(c, strcmp(mem_heap_name(0), "default") == 0 && mem_heap_budget(0) == 0);

    // A tagged view of the general allocator attributes to its tag, not the current owner.
    MemTag tagged; tagged.module = 5; tagged.category = cat_b; tagged.heap = heap;
    Allocator view;
    mem_general_tagged(view, tagged, "view");
    mem_set_owner(3);
    void* v = view.alloc(&view, 24, 16);
#if KV_DEV
    KV_CHECK(c, mem_stats_category(cat_b).live_bytes == 24 && mem_stats_module(5).live_allocs == 1);
    KV_CHECK(c, mem_stats_module(3).live_allocs == 0);
#endif
    KV_CHECK(c, mem_stats_heap(heap).live_bytes == 24 && mem_stats_heap(heap).live_allocs == 1);
    view.free(&view, v, 24);
#if KV_DEV
    KV_CHECK(c, mem_stats_category(cat_b).live_allocs == 0);
#endif
    KV_CHECK(c, mem_stats_heap(heap).live_bytes == 0);

    // General: default 16-byte alignment, explicit larger alignments, accounting per owner.
    void* a = mem_alloc(40);
    void* b = mem_alloc_aligned(100, 64);
    void* d = mem_alloc_aligned(8, 4096);
    KV_CHECK(c, a && ((usize)a & 15) == 0);
    KV_CHECK(c, b && ((usize)b & 63) == 0);
    KV_CHECK(c, d && ((usize)d & 4095) == 0);
    memset(a, 1, 40); memset(b, 2, 100); memset(d, 3, 8);
#if KV_DEV
    MemStats m3 = mem_stats_module(3);
    KV_CHECK(c, m3.live_allocs == 3 && m3.live_bytes == 148);
#endif
    KV_CHECK(c, mem_stats().live_allocs == before.live_allocs + 3);
    KV_CHECK(c, mem_stats().live_bytes == before.live_bytes + 148);

    // realloc keeps contents, the original tag and the original alignment.
    mem_set_owner(0);
    a = mem_realloc(a, 4000);
    KV_CHECK(c, a && ((u8*)a)[39] == 1);
    b = mem_realloc(b, 5000);
    KV_CHECK(c, b && ((usize)b & 63) == 0 && ((u8*)b)[99] == 2);
    KV_CHECK(c, mem_stats().live_allocs == before.live_allocs + 3);
    KV_CHECK(c, mem_stats().live_bytes == before.live_bytes + 148 - 40 + 4000 - 100 + 5000);

    // The frame sample is the cheap read: last frame's totals and the sampled peak.
    mem_sample(7);
    const MemSample& smp = mem_last_sample();
    KV_CHECK(c, smp.frame == 7 && smp.live_bytes == mem_stats().live_bytes);
    KV_CHECK(c, smp.peak_live_bytes >= smp.live_bytes);

    // A size that would wrap the header arithmetic is refused, not shrunk.
    KV_CHECK(c, mem_alloc(SIZE_MAX - 8) == nullptr);
    KV_CHECK(c, mem_alloc_aligned(SIZE_MAX - 100, 4096) == nullptr);

    mem_free(a); mem_free(b); mem_free(d);
#if KV_DEV
    KV_CHECK(c, mem_stats_module(3).live_allocs == 0 && mem_stats_module(3).live_bytes == 0);
#endif
    KV_CHECK(c, mem_stats().live_allocs == before.live_allocs);
    mem_sample(8);
    KV_CHECK(c, mem_last_sample().peak_live_bytes >= before.live_bytes + 148 - 40 + 4000 - 100 + 5000);
    mem_free(nullptr); // harmless

    // Page layer: reserve, commit, use, release.
    usize ps = os_page_size();
    KV_CHECK(c, ps >= 4096 && (ps & (ps - 1)) == 0);
    u8* range = (u8*)os_reserve(ps * 4);
    KV_CHECK(c, range != nullptr);
    KV_CHECK(c, os_commit(range + ps, ps));
    range[ps] = 7;
    KV_CHECK(c, range[ps] == 7);
    os_release(range, ps * 4);

    // Arena: alignment, growth across commit chunks, mark/release, reset, accounting.
    MemTag tag; tag.heap = heap; tag.category = cat_a;
    u64 heap_before = mem_stats_heap(heap).committed_bytes;
    Arena ar;
    KV_CHECK(c, arena_create(ar, 8u << 20, "test", tag));
    u8* p1 = (u8*)arena_alloc(ar, 10);
    u8* p2 = (u8*)arena_alloc(ar, 10, 64);
    KV_CHECK(c, p1 && p2 && p2 > p1 && ((usize)p2 & 63) == 0);
    usize mark = arena_mark(ar);
    u8* big = (u8*)arena_alloc(ar, 300 * 1024); // past the first 64 KB commit
    KV_CHECK(c, big != nullptr);
    big[300 * 1024 - 1] = 9;
    KV_CHECK(c, ar.committed >= 300 * 1024 && ar.committed <= ar.reserved);
    KV_CHECK(c, mem_stats_heap(heap).committed_bytes == heap_before + ar.committed);
#if KV_DEV
    KV_CHECK(c, mem_stats_category(cat_a).committed_bytes == ar.committed);
#endif
    arena_release(ar, mark);
    KV_CHECK(c, ar.used == mark && ar.peak >= 300 * 1024);
#if KV_DEV
    KV_CHECK(c, big[0] == 0xDD && big[300 * 1024 - 1] == 0xDD); // released space is poisoned
#endif
    KV_CHECK(c, (u8*)arena_alloc(ar, 1) == big); // the space is reused
    arena_reset(ar);
    KV_CHECK(c, ar.used == 0 && (u8*)arena_alloc(ar, 1) == ar.base);
    // Too big for the reservation fails instead of growing.
    KV_CHECK(c, arena_alloc(ar, 9u << 20) == nullptr);
    arena_destroy(ar);
    KV_CHECK(c, mem_stats_heap(heap).committed_bytes == heap_before);

    // Soft budget: the frame sample notices a heap past its budget, warns once, and
    // nothing fails.
    Arena fat;
    KV_CHECK(c, arena_create(fat, 4u << 20, "fat", tag));
    KV_CHECK(c, arena_alloc(fat, 2u << 20) != nullptr);
    mem_sample(9);
    KV_CHECK(c, mem_last_sample().heap_bytes[heap] >= 2u << 20);
    arena_destroy(fat);

    // Arena over a caller's buffer never commits and fails when full.
    alignas(16) u8 buf[256];
    Arena fixed;
    arena_from_buffer(fixed, buf, sizeof buf, "fixed", tag);
    KV_CHECK(c, arena_alloc(fixed, 200) == buf);
    KV_CHECK(c, arena_alloc(fixed, 100) == nullptr);
    arena_destroy(fixed);

    // Scratch and MemScope: nested temporaries release in order; an Array in scratch
    // frees for free.
    Arena* scratch = mem_scratch_arena();
    KV_CHECK(c, scratch != nullptr && mem_scratch() == &scratch->a);
    mem_scratch_reset();
    {
        MemScope outer;
        Array<u32> tmp = array_in<u32>(mem_scratch());
        for (u32 i = 0; i < 1000; ++i) tmp.push(i);
        KV_CHECK(c, tmp[999] == 999 && scratch->used > 0);
        usize used_outer = scratch->used;
        {
            MemScope inner;
            arena_alloc(*scratch, 5000);
            KV_CHECK(c, scratch->used > used_outer);
        }
        KV_CHECK(c, scratch->used == used_outer);
        tmp.free();
    }
    KV_CHECK(c, scratch->used == 0);
    KV_CHECK(c, mem_stats().live_allocs == before.live_allocs); // scratch is not general
}
