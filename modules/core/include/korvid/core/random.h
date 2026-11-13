// Korvid core: seeded random streams. Rng (PCG32) is the engine's: 16 bytes, fast, for the
// sim and everything else. Mt19937 is the Mersenne Twister, for matching another program's
// numbers: Python's random module (seeded), std::mt19937 and the reference code
// (seeded_std); it is 2.5 KB and slower, and nothing in the sim needs it. Both are plain
// data: a system or an entity owns one and advances it; the same seed gives the same
// sequence on every platform. Never shared between threads; never seeded from the clock
// inside the sim.
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

struct Mt19937 {
    u32 mt[624];
    u32 index = 625; // past the end until seeded

    // As Python seeds random.Random(seed) from a non-negative int: init_by_array of its
    // 32-bit words, least significant first. Then next(), unit(), below() and range() are
    // getrandbits(32), random(), randrange(n) and randint(lo, hi), number for number.
    static Mt19937 seeded(u64 seed) {
        u32 key[2] = {(u32)seed, (u32)(seed >> 32)};
        Mt19937 m;
        m.init_by_array(key, seed >> 32 ? 2 : 1);
        return m;
    }
    // As the reference code's init_genrand and std::mt19937 seed it (default 5489).
    static Mt19937 seeded_std(u32 seed = 5489u) {
        Mt19937 m;
        m.init_genrand(seed);
        return m;
    }

    u32 next() {
        if (index >= 624) twist();
        u32 y = mt[index++];
        y ^= y >> 11;
        y ^= (y << 7) & 0x9d2c5680u;
        y ^= (y << 15) & 0xefc60000u;
        return y ^ (y >> 18);
    }
    // [0, 1) with 53 bits, from two draws: Python's random().
    f64 unit() { u32 a = next() >> 5, b = next() >> 6; return ((f64)a * 67108864.0 + b) * (1.0 / 9007199254740992.0); }
    // [0, n), unbiased: the top bit_length(n) bits of a draw until one is under n, as
    // Python's randrange(n) (n up to 2^32 - 1; 0 gives 0).
    u32 below(u32 n) {
        if (n == 0) return 0;
        u32 k = 0; // n's bit length
        while (k < 32 && (n >> k) != 0) ++k;
        for (;;) {
            u32 r = next() >> (32 - k);
            if (r < n) return r;
        }
    }
    // [lo, hi], both included: Python's randint(lo, hi), for spans under 2^32.
    i64 range(i64 lo, i64 hi) { return lo + below((u32)(hi - lo + 1)); }

    void init_genrand(u32 s) {
        mt[0] = s;
        for (u32 i = 1; i < 624; ++i) mt[i] = 1812433253u * (mt[i - 1] ^ (mt[i - 1] >> 30)) + i;
        index = 624;
    }
    void init_by_array(const u32* key, u32 len) {
        init_genrand(19650218u);
        u32 i = 1, j = 0;
        for (u32 k = len > 624 ? len : 624; k; --k) {
            mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1664525u)) + key[j] + j;
            if (++i >= 624) { mt[0] = mt[623]; i = 1; }
            if (++j >= len) j = 0;
        }
        for (u32 k = 623; k; --k) {
            mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1566083941u)) - i;
            if (++i >= 624) { mt[0] = mt[623]; i = 1; }
        }
        mt[0] = 0x80000000u;
    }
    void twist() {
        for (u32 k = 0; k < 624; ++k) {
            u32 y = (mt[k] & 0x80000000u) | (mt[(k + 1) % 624] & 0x7fffffffu);
            mt[k] = mt[(k + 397) % 624] ^ (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
        }
        index = 0;
    }
};

} // namespace kv
