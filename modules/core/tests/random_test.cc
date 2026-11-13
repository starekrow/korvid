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

    // Mt19937 against the references: std::mt19937's 10000th output (the C++ standard's
    // own check), and Python's random.Random(0x5407e), random.Random(0) and a seed of two
    // words, number for number: getrandbits(32), random(), randint(3, 7), randrange(12).
    c.begin("random: Mt19937");
    Mt19937 s = Mt19937::seeded_std();
    u32 x = 0;
    for (u32 i = 0; i < 10000; ++i) x = s.next();
    KV_CHECK(c, x == 4123659995u);
    Mt19937 p = Mt19937::seeded(0x5407e);
    KV_CHECK(c, p.next() == 758137947u && p.next() == 132149255u && p.next() == 2818820525u && p.next() == 620935951u);
    p = Mt19937::seeded(0x5407e);
    KV_CHECK(c, p.unit() == 0x1.69822207e0700p-3 && p.unit() == 0x1.5007934940aecp-1 && p.unit() == 0x1.0df1fe1f501aap-1);
    const i64 ints[8] = {5, 6, 7, 6, 7, 5, 7, 4};
    bool ok = true;
    for (i64 v : ints) ok &= p.range(3, 7) == v;
    const u32 belows[8] = {8, 0, 5, 4, 5, 7, 8, 4};
    for (u32 v : belows) ok &= p.below(12) == v;
    KV_CHECK(c, ok);
    Mt19937 z = Mt19937::seeded(0), w = Mt19937::seeded((1ull << 40) | 5);
    KV_CHECK(c, z.next() == 3626764237u && z.next() == 1654615998u && z.next() == 3255389356u);
    KV_CHECK(c, w.next() == 2166296868u && w.next() == 2220160828u && w.next() == 1153647273u);
}
