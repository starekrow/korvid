// Korvid math: the scalar functions, with the same bits on every platform. The engine
// calls these and never the platform's libm: no <math.h>, so neither its cost to compile
// nor its answers, which differ from one C library to the next in the last bit or two.
//
// The exact ones (fabs, floor, ceil, trunc, round, fmod, sqrt, copysign, ldexp, frexp)
// have one right answer under IEEE 754 and are inline here. The transcendentals are in
// src/: each is computed in double with + - * / alone, from constants generated once
// (check/gen_constants.py), and rounded once at the end, so any IEEE 754 machine that
// does not fuse a multiply into an add gets the same result. The toolchains pin that
// (-ffp-contract=off, /fp:precise). The f32 ones are correctly rounded nearly always
// (check/README.md has the counts); the f64 ones are within a few units in the last place.
// Both overloads take their type: sin(0.5f) is f32, sin(0.5) is f64.
#pragma once
#include "korvid/core/types.h"
#if defined(_MSC_VER) && !defined(__clang__)
#include <emmintrin.h>
#endif

namespace kv {

constexpr f32 KV_PI = 3.14159265358979323846f;
constexpr f32 KV_INF = __builtin_huge_valf();
constexpr f32 KV_NAN = __builtin_nanf(""); // the canonical quiet NaN, 0x7fc00000

namespace math_bits {
inline u32 of(f32 x) { return __builtin_bit_cast(u32, x); }
inline u64 of(f64 x) { return __builtin_bit_cast(u64, x); }
inline f32 f32_of(u32 b) { return __builtin_bit_cast(f32, b); }
inline f64 f64_of(u64 b) { return __builtin_bit_cast(f64, b); }
// The NaN every function here returns, whatever NaN came in: x86's default NaN has the
// sign bit set and ARM's does not, so a NaN made by arithmetic differs between them.
inline f32 nan32() { return f32_of(0x7fc00000u); }
inline f64 nan64() { return f64_of(0x7ff8000000000000ull); }
// The nearest integer to v, |v| < 2^51, without a branch: adding 1.5 * 2^52 leaves no bits
// below the units, and taking it away again is exact. Ties go to even.
inline f64 nearest(f64 v) { return (v + 0x1.8p52) - 0x1.8p52; }
} // namespace math_bits

// ---- classification -----------------------------------------------------------
inline bool isnan(f32 x)    { return (math_bits::of(x) & 0x7fffffffu) > 0x7f800000u; }
inline bool isnan(f64 x)    { return (math_bits::of(x) & 0x7fffffffffffffffull) > 0x7ff0000000000000ull; }
inline bool isinf(f32 x)    { return (math_bits::of(x) & 0x7fffffffu) == 0x7f800000u; }
inline bool isinf(f64 x)    { return (math_bits::of(x) & 0x7fffffffffffffffull) == 0x7ff0000000000000ull; }
inline bool isfinite(f32 x) { return (math_bits::of(x) & 0x7f800000u) != 0x7f800000u; }
inline bool isfinite(f64 x) { return (math_bits::of(x) & 0x7ff0000000000000ull) != 0x7ff0000000000000ull; }
inline bool signbit(f32 x)  { return (math_bits::of(x) >> 31) != 0; }
inline bool signbit(f64 x)  { return (math_bits::of(x) >> 63) != 0; }

// ---- exact --------------------------------------------------------------------
inline f32 fabs(f32 x) { return math_bits::f32_of(math_bits::of(x) & 0x7fffffffu); }
inline f64 fabs(f64 x) { return math_bits::f64_of(math_bits::of(x) & 0x7fffffffffffffffull); }
inline f32 copysign(f32 x, f32 s) { return math_bits::f32_of((math_bits::of(x) & 0x7fffffffu) | (math_bits::of(s) & 0x80000000u)); }
inline f64 copysign(f64 x, f64 s) {
    return math_bits::f64_of((math_bits::of(x) & 0x7fffffffffffffffull) | (math_bits::of(s) & 0x8000000000000000ull));
}

// Toward zero. At 2^23 (2^52) and beyond, and for NaN and the infinities, x is already
// whole; below, the integer conversion truncates, and the sign of a zero is kept.
inline f32 trunc(f32 x) { return fabs(x) < 8388608.0f ? copysign((f32)(i32)x, x) : x; }
inline f64 trunc(f64 x) { return fabs(x) < 4503599627370496.0 ? copysign((f64)(i64)x, x) : x; }
// An instruction where the machine has one (ARM64; x86 from SSE4.1, which the baseline
// x86-64 build does not assume). Otherwise without a branch on the input, since a random
// sign mispredicts half the time: the correction, 0 or 1, is taken off the integer, where
// the compiler keeps it a subtraction (in float it makes a jump of it again). floor(x)
// has x's sign, zero included. ceil is -floor(-x).
#if !(defined(_MSC_VER) && !defined(__clang__)) && (defined(__aarch64__) || defined(__SSE4_1__))
inline f32 floor(f32 x) { return __builtin_floorf(x); }
inline f64 floor(f64 x) { return __builtin_floor(x); }
inline f32 ceil(f32 x)  { return __builtin_ceilf(x); }
inline f64 ceil(f64 x)  { return __builtin_ceil(x); }
#else
inline f32 floor(f32 x) {
    if (!(fabs(x) < 8388608.0f)) return x; // whole already, or NaN
    i32 i = (i32)x;
    return copysign((f32)(i - (i32)(x < (f32)i)), x);
}
inline f64 floor(f64 x) {
    if (!(fabs(x) < 4503599627370496.0)) return x;
    i64 i = (i64)x;
    return copysign((f64)(i - (i64)(x < (f64)i)), x);
}
inline f32 ceil(f32 x)  { return -floor(-x); }
inline f64 ceil(f64 x)  { return -floor(-x); }
#endif
// Halfway cases away from zero, as C's round. x - trunc(x) is exact; adding a zero of x's
// sign keeps the sign of a zero result.
inline f32 round(f32 x) { f32 t = trunc(x); return t + copysign((f32)(fabs(x - t) >= 0.5f), x); }
inline f64 round(f64 x) { f64 t = trunc(x); return t + copysign((f64)(fabs(x - t) >= 0.5), x); }
inline i64 lround(f32 x) { return (i64)round(x); }
inline i64 lround(f64 x) { return (i64)round(x); }

// The instruction: IEEE 754 has sqrt correctly rounded. The toolchains build with
// -fno-math-errno, so the builtin is the instruction alone and never a call into libm.
#if defined(_MSC_VER) && !defined(__clang__)
inline f32 sqrt(f32 x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }
inline f64 sqrt(f64 x) { __m128d v = _mm_set_sd(x); return _mm_cvtsd_f64(_mm_sqrt_sd(v, v)); }
#else
inline f32 sqrt(f32 x) { return __builtin_sqrtf(x); }
inline f64 sqrt(f64 x) { return __builtin_sqrt(x); }
#endif

f64 ldexp(f64 x, i32 n);           // x * 2^n, rounded once (into the subnormals too)
inline f32 ldexp(f32 x, i32 n) { return (f32)ldexp((f64)x, n); }
f64 frexp(f64 x, i32* e);          // x = m * 2^e, 0.5 <= |m| < 1; zero, inf and NaN give e = 0
f32 frexp(f32 x, i32* e);
f64 fmod(f64 x, f64 y);            // x - n*y, n = trunc(x/y), exact; the sign of x
inline f32 fmod(f32 x, f32 y) { return (f32)fmod((f64)x, (f64)y); } // exact in double, so exact once rounded

// ---- transcendental -------------------------------------------------------------
f32 sin(f32 x);
f32 cos(f32 x);
f32 tan(f32 x);
f32 asin(f32 x);
f32 acos(f32 x);
f32 atan(f32 x);
f32 atan2(f32 y, f32 x);
f32 exp(f32 x);
f32 exp2(f32 x);
f32 log(f32 x);
f32 log2(f32 x);
f32 log10(f32 x);
f32 pow(f32 x, f32 y);

f64 sin(f64 x);
f64 cos(f64 x);
f64 tan(f64 x);
f64 asin(f64 x);
f64 acos(f64 x);
f64 atan(f64 x);
f64 atan2(f64 y, f64 x);
f64 exp(f64 x);
f64 exp2(f64 x);
f64 log(f64 x);
f64 log2(f64 x);
f64 log10(f64 x);
f64 pow(f64 x, f64 y);

} // namespace kv
