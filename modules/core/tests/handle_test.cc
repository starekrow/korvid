#include "korvid/core/pool.h"
#include "korvid/core/test.h"

struct Thing { kv::u32 value; };

void test_handle(kv::TestCtx& c) {
    using namespace kv;
    c.begin("handle");

    Pool<Thing> pool;
    Handle a = pool.create(Thing{1});
    Handle b = pool.create(Thing{2});
    Handle d = pool.create(Thing{3});
    KV_CHECK(c, pool.live == 3);
    KV_CHECK(c, pool.get(a) && pool.get(a)->value == 1);
    KV_CHECK(c, pool.get(b) && pool.get(b)->value == 2);
    KV_CHECK(c, pool.get(d) && pool.get(d)->value == 3);

    // Null handle is never valid.
    KV_CHECK(c, !pool.valid(Handle{}));
    KV_CHECK(c, pool.get(Handle{}) == nullptr);

    // Destroy, then use the old handle: an error, never another entity's data. In dev
    // builds the slot is poisoned, so a pointer kept across the destroy reads 0xDD.
    Thing* b_ptr = pool.get(b);
    KV_CHECK(c, pool.destroy(b));
#if KV_DEV
    KV_CHECK(c, b_ptr->value == 0xDDDDDDDDu);
#else
    KV_UNUSED(b_ptr);
#endif
    KV_CHECK(c, pool.live == 2);
    KV_CHECK(c, pool.get(b) == nullptr);
    KV_CHECK(c, !pool.destroy(b)); // double destroy is reported, not fatal

    // The slot is reused with a new generation; the stale handle stays stale.
    Handle e = pool.create(Thing{4});
    KV_CHECK(c, e.index == b.index);
    KV_CHECK(c, e.gen != b.gen);
    KV_CHECK(c, pool.get(e) && pool.get(e)->value == 4);
    KV_CHECK(c, pool.get(b) == nullptr);

    // Out-of-range index with a plausible generation is also rejected.
    KV_CHECK(c, pool.get(Handle{99, 1}) == nullptr);

    // Addresses are stable across growth: the pool commits pages into one reservation.
    Thing* first = pool.get(a);
    for (u32 i = 0; i < 5000; ++i) pool.create(Thing{i});
    KV_CHECK(c, pool.get(a) == first && first->value == 1);
    KV_CHECK(c, pool.committed >= pool.len && pool.len == 5003);

    // Snapshot, mutate, restore: handles and staleness come back exactly.
    Handle f = pool.create(Thing{42});
    usize size = pool.snapshot_size();
    u8* shot = (u8*)mem_alloc(size);
    pool.snapshot(shot);
    pool.destroy(f);
    pool.get(a)->value = 1000;
    Handle g = pool.create(Thing{7}); // reuses f's slot with a new generation
    KV_CHECK(c, g.index == f.index && pool.get(f) == nullptr);
    KV_CHECK(c, pool.restore(shot));
    KV_CHECK(c, pool.get(f) && pool.get(f)->value == 42);
    KV_CHECK(c, pool.get(g) == nullptr);           // never existed in the snapshot
    KV_CHECK(c, pool.get(a)->value == 1);          // the mutation is undone
    KV_CHECK(c, pool.get(b) == nullptr);           // still stale
    KV_CHECK(c, pool.live == 5004);
    mem_free(shot);

    // Restoring into a fresh pool works too (the snapshot is relocatable).
    Pool<Thing> other;
    shot = (u8*)mem_alloc(size);
    pool.snapshot(shot);
    KV_CHECK(c, other.restore(shot));
    KV_CHECK(c, other.get(f) && other.get(f)->value == 42 && other.live == pool.live);
    mem_free(shot);
    other.free();

    pool.free();
    KV_CHECK(c, pool.live == 0 && pool.base == nullptr);
}
