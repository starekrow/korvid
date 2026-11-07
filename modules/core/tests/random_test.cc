#include "korvid/core/random.h"
#include "korvid/core/test.h"

void test_random(kv::TestCtx& c) {
    using namespace kv;
    c.begin("random");

    // Seeded streams repeat exactly and differ from each other.
    Rng a = Rng::seeded(42, 1), b = Rng::seeded(42, 1), d = Rng::seeded(42, 2), e = Rng::seeded(43, 1);
    bool same = true, diff_stream = false, diff_seed = false;
    for (u32 i = 0; i < 1000; ++i) {
        u32 x = a.next();
        same &= x == b.next();
        diff_stream |= x != d.next();
        diff_seed   |= x != e.next();
    }
    KV_CHECK(c, same && diff_stream && diff_seed);

    // PCG32 reference: the first output of seed 42, stream 54 is well known.
    Rng ref = Rng::seeded(42, 54);
    KV_CHECK(c, ref.next() == 0xa15c02b7u);

    // Ranges stay in bounds and spread out.
    Rng r = Rng::seeded(7, 0);
    bool in_unit = true, in_range = true, in_below = true;
    u32 buckets[8] = {};
    for (u32 i = 0; i < 10000; ++i) {
        f32 u = r.unit();
        in_unit &= u >= 0.0f && u < 1.0f;
        f32 v = r.range(-2.0f, 3.0f);
        in_range &= v >= -2.0f && v < 3.0f;
        u32 k = r.below(8);
        in_below &= k < 8;
        ++buckets[k];
    }
    KV_CHECK(c, in_unit && in_range && in_below);
    bool spread = true;
    for (u32 k = 0; k < 8; ++k) spread &= buckets[k] > 1000 && buckets[k] < 1500;
    KV_CHECK(c, spread);
    KV_CHECK(c, r.below(0) == 0);
}
