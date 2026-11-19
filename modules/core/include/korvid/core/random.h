// Korvid core: seeded random numbers: Rng for the game, Mt19937 for matching another
// program's numbers.
//
// A game's randomness has to repeat: the same seed and the same input must make the same
// game on every machine, or a replay, a test or a desync check means nothing. rand()
// cannot promise that. Its numbers differ between C libraries, and it is one hidden
// stream for the whole process, so a call from anywhere else (a library, another thread)
// shifts every number the game draws after it. Rng is a PCG32 instead: 16 bytes of plain
// data, fast, that you own, seed and advance yourself; the same seed gives the same
// numbers on every platform.
//
// A game's systems draw from the stream the engine keeps for their module, ctx.rng()
// (phase.h): seeded from the module's name and the engine's seed (--seed N), part of the
// state hash, seeded again by a fresh sim. Nothing to set up (examples/rocks/src/game.cc):
//
//   Velocity v{drift, ctx.rng().range(-1.5f, 1.5f)};   // a new rock's spin
//
// For determinism, the sim draws only from seeded streams: never rand(), the clock, or
// anything else that differs between runs. A stream that cosmetic code also draws from
// shifts with it, so effects keep an Rng of their own in the module's state, seeded once,
// unhashed (examples/arcfire keeps `fx` for its sparks and debris). An Rng kept in a sim
// component is hashed with its row (arcfire's `Chance`). One stream belongs to one thread
// at a time.
//
// Mt19937 is the Mersenne Twister, for reproducing numbers another program draws:
// Python's random module (seeded), std::mt19937 and the reference C code (seeded_std).
// It is 2.5 KB and slower, and nothing in the sim needs it; the script module's
// kv.random is one, so a tool ported from Python draws the same numbers.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// ---- Rng: the engine's stream ---------------------------------------------------------
// PCG32 (pcg-random.org): 64 bits of state, 32-bit outputs. Default-constructed it is
// PCG32's reference stream, valid and the same in every program; seeded() gives the
// game a sequence of its own.
struct Rng {
    u64 state = 0x853c49e6748fea9bull; // where the sequence is; every draw advances it
    u64 inc   = 0xda3e39cb94b95bdbull; // stream selector, always odd

    // A stream from `seed`. `stream` picks one of 2^63 sequences: the same seed on two
    // streams gives two unrelated sequences. As PCG32's reference pcg32_srandom_r.
    static Rng seeded(u64 seed, u64 stream = 0) {
        Rng r;
        r.state = 0;
        r.inc   = (stream << 1) | 1;
        r.next();
        r.state += seed;
        r.next();
        return r;
    }

    // The next 32 random bits; the draws below are built on it.
    u32 next() {
        u64 old = state;
        state = old * 6364136223846793005ull + inc;
        u32 xorshifted = (u32)(((old >> 18) ^ old) >> 27);
        u32 rot = (u32)(old >> 59);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }

    // [0, 1), in steps of 2^-24 (the precision of an f32 near 1).
    f32 unit() { return (f32)(next() >> 8) * (1.0f / 16777216.0f); } // [0, 1)
    // lo + (hi - lo) * unit(): from `lo` up to `hi`, `hi` not included but for rounding.
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * unit(); }
    // [0, n), from one draw by multiply and shift: each value's odds are off by at most
    // n / 2^32, far below what a game shows. below(0) is 0.
    u32 below(u32 n) { return n ? (u32)(((u64)next() * n) >> 32) : 0; } // [0, n)
    // True with probability `p`: 0 never, 1 always.
    bool chance(f32 p) { return unit() < p; }
};

// ---- Mt19937: matching another program ------------------------------------------------
// The 32-bit Mersenne Twister. Seed it with seeded() or seeded_std() before drawing;
// each draw names the Python or C++ call it matches.
struct Mt19937 {
    u32 mt[624];     // the state
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

    // The next 32 bits: std::mt19937's next output, Python's getrandbits(32).
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

    // ---- internal: the reference code's seeding and twist, which the calls above use ---
    // init_genrand(s) and init_by_array(key, len) as in the reference mt19937ar.c.
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
    // Refills the 624 words of state; next() calls it every 624 draws.
    void twist() {
        for (u32 k = 0; k < 624; ++k) {
            u32 y = (mt[k] & 0x80000000u) | (mt[(k + 1) % 624] & 0x7fffffffu);
            mt[k] = mt[(k + 397) % 624] ^ (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
        }
        index = 0;
    }
};

} // namespace kv
