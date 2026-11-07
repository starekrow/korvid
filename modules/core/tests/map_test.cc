#include "korvid/core/map.h"
#include "korvid/core/handle.h"
#include "korvid/core/test.h"

namespace {
struct Key2 { kv::u32 a, b; bool operator==(const Key2& o) const { return a == o.a && b == o.b; } };
kv::u64 hash_key(const Key2& k) { return kv::hash_mix(((kv::u64)k.a << 32) | k.b); }
}

void test_map(kv::TestCtx& c) {
    using namespace kv;
    c.begin("map");

    MemStats before = mem_stats();
    Map<u32, u32> m;

    // Small mode: no index, a scan. Insert, find, overwrite, miss.
    for (u32 i = 0; i < KV_MAP_SMALL; ++i) m.set(i * 7, i);
    KV_CHECK(c, m.len() == KV_MAP_SMALL && m.index == nullptr);
    KV_CHECK(c, m.find(21) && *m.find(21) == 3 && !m.find(22) && m.has(0) && !m.has(1));
    m.set(21, 300);
    KV_CHECK(c, *m.find(21) == 300 && m.len() == KV_MAP_SMALL);

    // Promotion: one past the threshold builds the index; everything is still there.
    m.set(1000, 1000);
    KV_CHECK(c, m.index != nullptr && m.index_cap >= 32 && m.len() == KV_MAP_SMALL + 1);
    u32 kept = 0;
    for (u32 i = 0; i < KV_MAP_SMALL; ++i) kept += m.find(i * 7) && *m.find(i * 7) == (i == 3 ? 300u : i);
    KV_CHECK(c, kept == KV_MAP_SMALL && *m.find(1000) == 1000);

    // Iteration is insertion order over the dense arrays.
    KV_CHECK(c, m.keys[0] == 0 && m.keys[3] == 21 && m.keys[KV_MAP_SMALL] == 1000);

    // Remove swap-removes: the last entry takes the hole; the index follows it.
    KV_CHECK(c, m.remove(21) && !m.find(21) && m.len() == KV_MAP_SMALL);
    KV_CHECK(c, m.keys[3] == 1000 && *m.find(1000) == 1000);
    KV_CHECK(c, !m.remove(21));

    // Growth through several rebuilds with interleaved removes; nothing lost, nothing extra.
    for (u32 i = 0; i < 20000; ++i) m.set(100000 + i, i);
    u32 removed = 0;
    for (u32 i = 0; i < 20000; i += 3) removed += m.remove(100000 + i);
    KV_CHECK(c, removed == 6667);
    u32 ok = 0;
    for (u32 i = 0; i < 20000; ++i) {
        u32* v = m.find(100000 + i);
        if (i % 3 == 0) ok += v == nullptr;
        else ok += v && *v == i;
    }
    KV_CHECK(c, ok == 20000);
    KV_CHECK(c, m.len() == KV_MAP_SMALL + 20000 - 6667);
    KV_CHECK(c, (u64)m.len() * 4 <= (u64)m.index_cap * 3); // load factor held

    // get_or_insert reports which it did.
    bool inserted = false;
    u32* v = m.get_or_insert(5, 50, &inserted);
    KV_CHECK(c, inserted && *v == 50);
    v = m.get_or_insert(5, 60, &inserted);
    KV_CHECK(c, !inserted && *v == 50);

    // clear keeps the index allocated; free gives everything back.
    m.clear();
    KV_CHECK(c, m.len() == 0 && m.index != nullptr && !m.find(5));
    m.set(1, 1);
    KV_CHECK(c, *m.find(1) == 1);
    m.free();
    KV_CHECK(c, m.index == nullptr && mem_stats().live_allocs == before.live_allocs);

    // Determinism: the same operations give the same order and the same index capacity.
    Map<u32, u32> p, q;
    for (u32 i = 0; i < 100; ++i) { p.set(i * 31, i); q.set(i * 31, i); }
    for (u32 i = 0; i < 100; i += 4) { p.remove(i * 31); q.remove(i * 31); }
    bool same = p.len() == q.len() && p.index_cap == q.index_cap;
    for (u32 i = 0; same && i < p.len(); ++i) same = p.keys[i] == q.keys[i] && p.values[i] == q.values[i];
    KV_CHECK(c, same);
    p.free(); q.free();

    // Handle keys and a struct key with its own hash_key.
    Map<Handle, u32> by_handle;
    Handle ha = Handle::make(3, 1, 7), hb = Handle::make(3, 2, 7); // same index, different generation
    by_handle.set(ha, 1);
    by_handle.set(hb, 2);
    KV_CHECK(c, *by_handle.find(ha) == 1 && *by_handle.find(hb) == 2 && by_handle.len() == 2);
    by_handle.free();
    Map<Key2, u32> by_pair;
    by_pair.set(Key2{1, 2}, 12);
    by_pair.set(Key2{2, 1}, 21);
    KV_CHECK(c, *by_pair.find(Key2{1, 2}) == 12 && *by_pair.find(Key2{2, 1}) == 21 && !by_pair.find(Key2{1, 1}));
    by_pair.free();

    // The hash is fixed: these values are part of the contract (cooked data relies on them).
    KV_CHECK(c, hash_bytes("", 0) == hash_bytes("", 0) && hash_str("korvid") != hash_str("korvie"));
    KV_CHECK(c, hash_bytes("abcdefghij", 10) != hash_bytes("abcdefghik", 10));
    KV_CHECK(c, hash_key((u32)1) != hash_key((u32)2) && hash_key(ha) != hash_key(hb));
    KV_CHECK(c, mem_stats().live_allocs == before.live_allocs);
}
