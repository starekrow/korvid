// Korvid core: a seeded random stream (PCG32). Plain data: a system or an entity owns
// one and advances it; the same seed gives the same sequence on every platform. Never
// shared between threads; never seeded from the clock inside the sim.
#pragma once
#include "korvid/core/types.h"

namespace kv {

struct Rng {
    u64 state = 0x853c49e6748fea9bull;
    u64 inc   = 0xda3e39cb94b95bdbull; // stream selector, always odd

    static Rng seeded(u64 seed, u64 stream = 0) {
        Rng r;
        r.state = 0;
        r.inc   = (stream << 1) | 1;
        r.next();
        r.state += seed;
        r.next();
        return r;
    }

    u32 next() {
        u64 old = state;
        state = old * 6364136223846793005ull + inc;
        u32 xorshifted = (u32)(((old >> 18) ^ old) >> 27);
        u32 rot = (u32)(old >> 59);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }

    f32 unit() { return (f32)(next() >> 8) * (1.0f / 16777216.0f); } // [0, 1)
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * unit(); }
    u32 below(u32 n) { return n ? (u32)(((u64)next() * n) >> 32) : 0; } // [0, n)
    bool chance(f32 p) { return unit() < p; }
};

} // namespace kv
