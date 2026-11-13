// Korvid math: the default f32 transcendentals. Each works in double with + - * / alone,
// as the _hp ones do, but on less: a polynomial short enough for an f32's 24 bits
// (Chebyshev-fitted, check/gen_constants.py), a table where glibc has one (exp's 2^(j/64),
// log's 1/c), a reduction with two parts of pi/2. What they compute is near 2^-44 of the
// true value, so the rounding to f32 goes wrong on a few dozen of the 2^32 inputs and
// never by more than an ulp (check/README.md). Choices are selects, not branches: an
// angle's quadrant or a sign is as good as random. Arguments outside the common range (a float of 2^20 or more for the
// trig, the zeros, infinities and NaN of atan2 and pow) go to the _hp functions.
#include "korvid/math/scalar.h"
#include "constants.h"

namespace kv {
namespace {
using namespace math_constants;
using math_bits::select;

// c[0] + c[1] z + ... + c[N-1] z^(N-1), two coefficients a step: (c[0] + c[1] z) +
// z^2 ((c[2] + c[3] z) + z^2 (...)). The pairs are independent, so the chain is half as
// long as Horner's. Written out by the template: as a loop the compiler keeps the loop.
template <i32 I = 0, i32 N> f64 poly2(const f64 (&c)[N], f64 z, f64 z2) {
    if constexpr (I == N - 1) return c[I];
    else if constexpr (I == N - 2) return c[I] + z * c[I + 1];
    else return (c[I] + z * c[I + 1]) + z2 * poly2<I + 2>(c, z, z2);
}
template <i32 N> f64 poly(const f64 (&c)[N], f64 z) { return poly2(c, z, z * z); }

// |x| < 2^20: x = n*pi/2 + r. k * kPio2_1 is exact (33 bits by 20), and what kPio2_1t
// leaves out, times k, is under 2^-67: small beside the r of any float below 2^20 near a
// multiple of pi/2 (check/run.sh tries every one).
f64 reduce_f32(f64 x, i32* n) {
    f64 k = math_bits::nearest(x * kInvPio2);
    *n = (i32)((i64)k & 3);
    return (x - k * kPio2_1) - k * kPio2_1t;
}
f64 sin_poly(f64 r, f64 z) { return r + r * z * poly(kSinP, z); }
f64 cos_poly(f64 z) { return 1.0 + z * poly(kCosP, z); }

// The angle of (ax, ay), both > 0, in [0, pi/2], with one division and no select: the
// nearest of k pi/8 (k from four comparisons with tan((2k+1) pi/16)), plus the atan of
// what is left, u = tan(angle - k pi/8) = (ay cos - ax sin) / (ax cos + ay sin), |u| <=
// tan(pi/16). At k = 0 and 4 the sum and difference are ay and ax themselves, so small
// angles keep every bit.
f64 angle(f64 ay, f64 ax) {
    i32 k = (i32)(ay > kAtanBound[0] * ax) + (i32)(ay > kAtanBound[1] * ax) + (i32)(ay > kAtanBound[2] * ax) +
            (i32)(ay > kAtanBound[3] * ax);
    f64 c = kAtanCos[k], s = kAtanSin[k];
    f64 u = (ay * c - ax * s) / (ax * c + ay * s);
    f64 z = u * u;
    return kAtanTheta[k] + (u + u * z * poly(kAtanP, z));
}

// asin s, 0 <= s <= 1/2 (w = s^2).
f64 asin_poly(f64 s, f64 w) { return s + s * w * poly(kAsinP, w); }

// 2^t for |t| < 2^44 (as an f32 sees it: overflow and underflow are the rounding's):
// t = (k + r)/64, |r| <= 1/2; 2^(k/64) from the table, its exponent added to the bits.
f64 exp2_64(f64 t64) {
    f64 kd = math_bits::nearest(t64);
    f64 r = t64 - kd; // exact
    i64 k = (i64)kd;
    f64 s = math_bits::f64_of(math_bits::of(kExp2T[k & 63]) + ((u64)(k >> 6) << 52));
    return s + s * (r * poly(kExpP, r));
}

// x > 0 finite: x = 2^k z, z in [0x1.66p-1, 0x1.66p+0), cut into 64 cells by the bits
// under the exponent; r = z/c - 1, exact (z has 24 bits, 1/c 29). Returns log(1 + r).
f64 log_cell(f32 x, i32* k, i32* cell) {
    u32 u = math_bits::of(x);
    if (u < 0x00800000u) u = math_bits::of(x * 0x1p23f) - (23u << 23); // subnormal: scaled up
    u32 t = u - 0x3f330000u;
    *cell = (i32)((t >> 17) & 63);
    *k = (i32)t >> 23;
    f64 z = (f64)math_bits::f32_of(u - (t & 0xff800000u));
    f64 r = z * kLogInvC[*cell] - 1.0;
    return r + r * r * poly(kLog1pP, r);
}
// What log does not take: zero, below zero, inf, NaN.
bool log_ordinary(f32 x) { return x > 0.0f && x < KV_INF; }
f32 log_special(f32 x) {
    if (x == 0.0f) return -KV_INF;
    if (x == KV_INF) return x;
    return math_bits::nan32();
}
} // namespace

namespace math_raw {
// Below 2^-12 sin and tan are x, rounded (the cube term is under half an ulp).
f64 sin(f32 x) {
    u32 ax = math_bits::of(x) & 0x7fffffffu;
    if (ax < 0x39800000u) return (f64)x;
    if (ax >= 0x49800000u) return kv::sin((f64)x);
    i32 n;
    f64 r = reduce_f32((f64)x, &n), z = r * r;
    u64 s = math_bits::of(sin_poly(r, z)), c = math_bits::of(cos_poly(z));
    u64 odd = (u64)0 - (u64)(n & 1);
    return math_bits::f64_of(((s & ~odd) | (c & odd)) ^ ((u64)(n & 2) << 62));
}
f64 cos(f32 x) { // sin(x + pi/2): the next quadrant
    u32 ax = math_bits::of(x) & 0x7fffffffu;
    if (ax >= 0x49800000u) return kv::cos((f64)x);
    i32 n;
    f64 r = reduce_f32((f64)x, &n), z = r * r;
    n += 1;
    u64 s = math_bits::of(sin_poly(r, z)), c = math_bits::of(cos_poly(z));
    u64 odd = (u64)0 - (u64)(n & 1);
    return math_bits::f64_of(((s & ~odd) | (c & odd)) ^ ((u64)(n & 2) << 62));
}
f64 tan(f32 x) {
    u32 ax = math_bits::of(x) & 0x7fffffffu;
    if (ax < 0x39800000u) return (f64)x;
    if (ax >= 0x49800000u) return kv::tan((f64)x);
    i32 n;
    f64 r = reduce_f32((f64)x, &n), z = r * r;
    u64 s = math_bits::of(sin_poly(r, z)), c = math_bits::of(cos_poly(z));
    u64 odd = (u64)0 - (u64)(n & 1);
    f64 num = math_bits::f64_of((s & ~odd) | (c & odd)), den = math_bits::f64_of((c & ~odd) | (s & odd));
    return math_bits::f64_of(math_bits::of(num / den) ^ (odd & 0x8000000000000000ull));
}

f64 atan(f32 x) {
    u32 ax = math_bits::of(x) & 0x7fffffffu;
    if (ax < 0x39800000u) return (f64)x; // atan x = x - x^3/3: x, rounded
    if (ax > 0x7f800000u) return math_bits::nan64();
    if (ax == 0x7f800000u) return copysign(kPio2, (f64)x); // inf: the sector's cos is 0, and inf * 0 is NaN
    return copysign(angle(fabs((f64)x), 1.0), (f64)x);
}
f64 atan2(f32 y, f32 x) {
    u32 ay = math_bits::of(y) & 0x7fffffffu, ax = math_bits::of(x) & 0x7fffffffu;
    if (ay == 0 || ax == 0 || ay >= 0x7f800000u || ax >= 0x7f800000u) return kv::atan2((f64)y, (f64)x);
    f64 a = angle(fabs((f64)y), fabs((f64)x));
    a = select(x < 0.0f, kPi - a, a);
    return copysign(a, (f64)y);
}
// Up to 1/2, asin's polynomial in |x|; past it, asin x = pi/2 - 2 asin(sqrt((1 - x)/2)),
// where 1 - x is exact. w is picked apart from s, so the polynomial in w does not wait for
// the square root.
f64 asin(f32 x) {
    f64 a = fabs((f64)x);
    if (!(a <= 1.0)) return math_bits::nan64();
    if (a < 0x1p-12) return (f64)x;
    bool big = a > 0.5;
    f64 w = select(big, (1.0 - a) * 0.5, a * a);
    f64 p = asin_poly(select(big, sqrt(w), a), w);
    return copysign(select(big, kPio2 - 2.0 * p, p), (f64)x);
}
// pi/2 - asin x up to 1/2; past it 2 asin(sqrt((1 - x)/2)), or pi less that below -1/2.
f64 acos(f32 x) {
    f64 xd = (f64)x, a = fabs(xd);
    if (!(a <= 1.0)) return math_bits::nan64();
    bool big = a > 0.5;
    f64 w = select(big, (1.0 - a) * 0.5, a * a);
    f64 p = asin_poly(select(big, sqrt(w), a), w);
    f64 small = kPio2 - copysign(p, xd);
    f64 large = select(xd < 0.0, kPi - 2.0 * p, 2.0 * p);
    return select(big, large, small);
}

// Past 128 (or -150) the result is inf (or 0) once rounded; past 2^44 the reduction's
// integer would not hold, so those are said here.
f64 exp(f32 x) {
    if (!(fabs(x) < 200.0f)) return isnan(x) ? math_bits::nan64() : (x > 0.0f ? (f64)KV_INF : 0.0);
    return exp2_64((f64)x * kInvLn2N);
}
f64 exp2(f32 x) {
    if (!(fabs(x) < 300.0f)) return isnan(x) ? math_bits::nan64() : (x > 0.0f ? (f64)KV_INF : 0.0);
    return exp2_64((f64)x * 64.0);
}

f64 log(f32 x) {
    if (!log_ordinary(x)) return (f64)log_special(x);
    i32 k, cell;
    f64 l = log_cell(x, &k, &cell);
    return (((f64)k * kLn2 + kLogC[cell]) + l);
}
f64 log2(f32 x) {
    if (!log_ordinary(x)) return (f64)log_special(x);
    i32 k, cell;
    f64 l = log_cell(x, &k, &cell);
    return (((f64)k + kLog2C[cell]) + l * kInvLn2);
}
f64 log10(f32 x) {
    if (!log_ordinary(x)) return (f64)log_special(x);
    i32 k, cell;
    f64 l = log_cell(x, &k, &cell);
    return ((((f64)k * kLn2 + kLogC[cell]) + l) * kInvLn10);
}

// 2^(y log2 x) in one double: log2 x to about 2^-50, which y log2 x (under 150 short of
// an overflow) keeps to 2^-42 of the result. x positive and normal, y finite and not zero,
// by two unsigned ranges; C's table (zeros, ones, infinities, a negative or subnormal x)
// is pow_hp's. x = 1 needs no case: log2 1 is 0 and 2^0 is 1, exactly.
f64 pow(f32 x, f32 y) {
    u32 ux = math_bits::of(x), uy = math_bits::of(y);
    if (ux - 0x00800000u >= 0x7f000000u || (uy << 1) - 1u >= 0xff000000u - 1u) return kv::pow((f64)x, (f64)y);
    i32 k, cell;
    f64 l = log_cell(x, &k, &cell);
    f64 t = (f64)y * (((f64)k + kLog2C[cell]) + l * kInvLn2);
    if (!(fabs(t) < 300.0)) return t > 0.0 ? (f64)KV_INF : 0.0;
    return exp2_64(t * 64.0);
}

} // namespace math_raw

// Each rounds its raw double once; a NaN is the canonical one whatever the conversion keeps.
namespace {
f32 to_f32(f64 d) { return isnan(d) ? math_bits::nan32() : (f32)d; }
} // namespace
f32 sin(f32 x) { return to_f32(math_raw::sin(x)); }
f32 cos(f32 x) { return to_f32(math_raw::cos(x)); }
f32 tan(f32 x) { return to_f32(math_raw::tan(x)); }
f32 asin(f32 x) { return to_f32(math_raw::asin(x)); }
f32 acos(f32 x) { return to_f32(math_raw::acos(x)); }
f32 atan(f32 x) { return to_f32(math_raw::atan(x)); }
f32 atan2(f32 y, f32 x) { return to_f32(math_raw::atan2(y, x)); }
f32 exp(f32 x) { return to_f32(math_raw::exp(x)); }
f32 exp2(f32 x) { return to_f32(math_raw::exp2(x)); }
f32 log(f32 x) { return to_f32(math_raw::log(x)); }
f32 log2(f32 x) { return to_f32(math_raw::log2(x)); }
f32 log10(f32 x) { return to_f32(math_raw::log10(x)); }
f32 pow(f32 x, f32 y) { return to_f32(math_raw::pow(x, y)); }

} // namespace kv
