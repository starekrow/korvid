// The grouped broadphase against brute force: random scenes of circles and boxes of mixed
// sizes (a few far larger than the rest, as a boss among bullets), on layers and masks
// drawn from a game-like set, must give exactly the pairs a test of every pair gives, and
// look at no pair whose masks cannot meet.
#include "../src/collision_internal.h"
#include "korvid/core/arena.h"
#include "korvid/core/random.h"
#include "korvid/core/test.h"

namespace {

using namespace kv;

struct Sig { u32 layer, mask; };
// Ship, ring, shot, air, ground, bullet (twice: a bullet is the commonest), and a group that
// meets itself, as a JRPG's actors do.
constinit const Sig k_sigs[] = {{1, 16 | 4}, {32, 16}, {2, 4 | 8}, {4, 2 | 1}, {8, 2}, {16, 1 | 32}, {16, 1 | 32}, {128, 128}};

} // namespace

void test_pairs(kv::TestCtx& c) {
    c.begin("pairs");
    Rng r = Rng::seeded(0x9a125, 1);
    bool all_same = true, never_masked = true;
    u32 total_pairs = 0;
    for (u32 scene = 0; scene < 60; ++scene) {
        MemScope scope;
        Array<CollItem> items = array_in<CollItem>(mem_scratch());
        u32 n = 50 + r.below(600);
        f32 extent = 100.0f + r.range(0.0f, 900.0f); // dense to sparse
        for (u32 i = 0; i < n; ++i) {
            CollItem it{};
            it.h = Handle{(u64)(i + 1) * 0x100000001ull + scene};
            // Some clumps (a ring of bullets fired from one point), the rest scattered.
            if (r.chance(0.3f)) it.p = point2{r.range(-5.0f, 5.0f), r.range(-5.0f, 5.0f)};
            else it.p = point2{r.range(-extent, extent), r.range(-extent, extent)};
            const Sig& s = k_sigs[r.below(KV_COUNTOF(k_sigs))];
            it.layer = s.layer;
            it.mask = s.mask;
            bool big = r.chance(0.01f);
            if (r.chance(0.7f)) { it.shape = (u8)ColliderShape::Circle; it.r = big ? r.range(80.0f, 220.0f) : r.range(2.0f, 20.0f); }
            else {
                it.shape = (u8)ColliderShape::Box;
                it.half = big ? vec2{r.range(60.0f, 200.0f), r.range(40.0f, 120.0f)} : vec2{r.range(2.0f, 16.0f), r.range(2.0f, 16.0f)};
                it.r = length(it.half);
            }
            items.push(it);
        }
        Array<CollPair> got = array_in<CollPair>(mem_scratch());
        CollPairStats st;
        collision_find_pairs(Span<const CollItem>(items.data, items.len), got, &st);
        // Brute force, and the count of pairs that may meet at all.
        Array<CollPair> want = array_in<CollPair>(mem_scratch());
        u32 meetable = 0;
        for (u32 i = 0; i < items.len; ++i)
            for (u32 j = i + 1; j < items.len; ++j) {
                if (!collision_meets(items[i].layer, items[i].mask, items[j].layer, items[j].mask)) continue;
                ++meetable;
                if (!collision_items_overlap(items[i], items[j])) continue;
                CollPair p{items[i].h.bits, items[j].h.bits};
                if (p.a > p.b) { u64 t = p.a; p.a = p.b; p.b = t; }
                want.push(p);
            }
        // Sorted the same way, then compared in order.
        for (u32 i = 1; i < want.len; ++i)
            for (u32 j = i; j > 0 && (want[j - 1].a > want[j].a || (want[j - 1].a == want[j].a && want[j - 1].b > want[j].b)); --j) {
                CollPair t = want[j]; want[j] = want[j - 1]; want[j - 1] = t;
            }
        bool same = got.len == want.len;
        for (u32 i = 0; same && i < got.len; ++i) same = got[i].a == want[i].a && got[i].b == want[i].b;
        all_same = all_same && same;
        never_masked = never_masked && st.tested <= meetable;
        total_pairs += want.len;
    }
    KV_CHECK(c, all_same);
    KV_CHECK(c, never_masked);
    KV_CHECK(c, total_pairs > 1000); // the scenes overlap enough to mean something
}
