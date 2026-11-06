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

    // Destroy, then use the old handle: an error, never another entity's data.
    KV_CHECK(c, pool.destroy(b));
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

    pool.free();
    KV_CHECK(c, pool.live == 0);
}
