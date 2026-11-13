// Korvid math: exp, exp2, log, log2, log10 and pow. exp takes x to k*ln2 + r, |r| <= ln2/2
// (ln 2 in two parts), and e^r is a series; log takes x to 2^e * m, m in [sqrt(1/2),
// sqrt(2)), and log m = 2 atanh((m-1)/(m+1)) is a series. pow is 2^(y log2 x), with
// log2 x and the product carried in two doubles (y log2 x needs about 70 bits when it is
// large), or for an integer y of 64 or less, repeated squaring in two doubles, which is
// exact when the result is (stb_vorbis counts on pow(r, dim) being exact). The f32 functions are the f64
// ones rounded once more.
#include "korvid/math/scalar.h"
#include "constants.h"

namespace kv {
namespace {
using namespace math_constants;

// e^r - 1 for |r| <= ln2/2 (a little more is fine), to r^13: what is left out is under
// 2^-57 of e^r.
f64 expm1_series(f64 r) {
    f64 p = kInvFact[13];
    for (i32 n = 12; n >= 1; n--) p = kInvFact[n] + r * p;
    return r * p;
}
f64 exp_series(f64 r) { return 1.0 + expm1_series(r); }

// A value carried in two doubles, hi + lo, |lo| at most half an ulp of hi. Sums and
// products made exact with + - * alone (Knuth, Dekker), so no fused multiply-add is needed
// and none changes the result.
struct dd { f64 hi, lo; };
dd two_sum(f64 a, f64 b) {
    f64 s = a + b, bb = s - a;
    return dd{s, (a - (s - bb)) + (b - bb)};
}
dd fast_two_sum(f64 a, f64 b) { // |a| >= |b|
    f64 s = a + b;
    return dd{s, b - (s - a)};
}
void split(f64 a, f64* hi, f64* lo) { // |a| < 2^995
    f64 c = 134217729.0 * a; // 2^27 + 1
    *hi = c - (c - a);
    *lo = a - *hi;
}
dd two_prod(f64 a, f64 b) {
    f64 p = a * b, ah, al, bh, bl;
    split(a, &ah, &al);
    split(b, &bh, &bl);
    return dd{p, ((ah * bh - p) + ah * bl + al * bh) + al * bl};
}

dd mul(dd a, dd b) {
    dd p = two_prod(a.hi, b.hi);
    return fast_two_sum(p.hi, p.lo + (a.hi * b.lo + a.lo * b.hi));
}

// x > 0 finite: x = 2^e * m, m in [sqrt(1/2), sqrt(2)); returns m - 1, which is exact.
f64 log_arg(f64 x, i32* e) {
    u64 b = math_bits::of(x);
    i32 ex = (i32)((b >> 52) & 0x7ff);
    i32 adj = 0;
    if (ex == 0) { // subnormal: scaled into the normals first
        b = math_bits::of(x * 0x1p54);
        ex = (i32)((b >> 52) & 0x7ff);
        adj = -54;
    }
    u64 mbits = (b & 0xfffffffffffffull) | 0x3ff0000000000000ull; // m in [1, 2)
    i32 k = ex - 1023 + adj;
    u64 over = mbits > 0x3ff6a09e667f3bcdull; // past sqrt(2): halved, without a branch
    mbits -= over << 52;
    k += (i32)over;
    *e = k;
    return math_bits::f64_of(mbits) - 1.0;
}
// 2s(z/3 + z^2/5 + ... + z^11/23), z = s^2: log(1 + f) less 2s, s = f / (2 + f). |s| <=
// 0.172: the next term is under 2^-56.
f64 log_tail(f64 s) {
    f64 z = s * s;
    f64 q = kInvOdd[11];
    for (i32 n = 10; n >= 1; n--) q = kInvOdd[n] + z * q;
    return 2.0 * s * z * q;
}
// log m, m = 1 + f.
f64 log_reduce(f64 x, i32* e) {
    f64 f = log_arg(x, e);
    f64 s = f / (2.0 + f);
    return 2.0 * s + log_tail(s);
}
// log2 x as hi + lo, to about 2^-66 of itself (a y log2 x of 1000 needs 2^-64): s and its
// rounding error, 2s^3/3 in two doubles too, the rest of the series in one, then 1/ln2 in
// two parts.
dd log2_dd(f64 x) {
    i32 e;
    f64 f = log_arg(x, &e);
    dd d = two_sum(2.0, f);
    f64 sh = f / d.hi;
    dd p = two_prod(sh, d.hi);
    f64 sl = (((f - p.hi) - p.lo) - sh * d.lo) / d.hi;
    dd s2 = two_prod(sh, sh);
    dd s3 = two_prod(s2.hi, sh);
    s3.lo += s2.lo * sh;
    dd t1 = two_prod(s3.hi, kTwoThirds);
    t1.lo += s3.hi * kTwoThirdsLo + s3.lo * kTwoThirds;
    f64 z = s2.hi;
    f64 q = kInvOdd[11];
    for (i32 n = 10; n >= 2; n--) q = kInvOdd[n] + z * q;
    f64 rest = 2.0 * sh * z * z * q; // 2s^5/5 + ...
    dd a = fast_two_sum(2.0 * sh, t1.hi);
    // 2 sl, and what sl adds to the cube, 2 s^2 sl: the rest of (sh + sl)'s terms are past 2^-100.
    dd l = fast_two_sum(a.hi, a.lo + ((t1.lo + 2.0 * sl + 2.0 * z * sl) + rest)); // log m
    dd m = two_prod(l.hi, kInvLn2);
    m.lo += l.hi * kInvLn2Lo + l.lo * kInvLn2;
    dd t = two_sum((f64)e, m.hi);
    return fast_two_sum(t.hi, t.lo + m.lo);
}
// 2^(t.hi + t.lo): k the nearest integer, the rest times ln 2 in two parts, e^r as
// e^r.hi * (1 + r.lo).
f64 exp2_dd(dd t) {
    const f64 inf = math_bits::f64_of(0x7ff0000000000000ull);
    if (t.hi >= 1024.0) return inf;
    if (t.hi < -1080.0) return 0.0;
    f64 k = math_bits::nearest(t.hi);
    f64 f = t.hi - k; // exact
    dd r = two_prod(f, kLn2);
    r.lo += f * kLn2Lo + t.lo * kLn2;
    dd rr = fast_two_sum(r.hi, r.lo);
    f64 w = expm1_series(rr.hi);
    return ldexp(1.0 + (w + rr.lo * (1.0 + w)), (i32)k);
}

bool is_integer(f64 y) { return trunc(y) == y; }
bool is_odd_integer(f64 y) { return fabs(y) < 0x1p53 && is_integer(y) && ((i64)y & 1) != 0; }
} // namespace

f64 exp(f64 x) {
    if (isnan(x)) return math_bits::nan64();
    if (x > 0x1.62e42fefa39efp+9) return math_bits::f64_of(0x7ff0000000000000ull); // past ln(DBL_MAX)
    if (x < -0x1.74910d52d3052p+9) return 0.0;                                     // under half the least subnormal
    f64 k = math_bits::nearest(x * kInvLn2);
    f64 r = (x - k * kLn2_1) - k * kLn2_2;
    return ldexp(exp_series(r), (i32)k);
}

f64 exp2(f64 x) {
    if (isnan(x)) return math_bits::nan64();
    if (x >= 1024.0) return math_bits::f64_of(0x7ff0000000000000ull);
    if (x < -1075.0) return 0.0;
    f64 k = math_bits::nearest(x);
    return ldexp(exp_series((x - k) * kLn2), (i32)k); // x - k is exact
}

// Zero is -inf, below zero NaN, +inf itself.
f64 log(f64 x) {
    if (isnan(x) || x < 0.0) return math_bits::nan64();
    if (x == 0.0) return -math_bits::f64_of(0x7ff0000000000000ull);
    if (isinf(x)) return x;
    i32 e;
    f64 lm = log_reduce(x, &e);
    return (f64)e * kLn2_1 + ((f64)e * kLn2_2 + lm); // e * kLn2_1 is exact
}
// e plus what m adds, so a power of two is exactly its exponent.
f64 log2(f64 x) {
    if (isnan(x) || x < 0.0) return math_bits::nan64();
    if (x == 0.0) return -math_bits::f64_of(0x7ff0000000000000ull);
    if (isinf(x)) return x;
    i32 e;
    f64 lm = log_reduce(x, &e);
    return (f64)e + lm * kInvLn2;
}
f64 log10(f64 x) {
    if (isnan(x) || x < 0.0) return math_bits::nan64();
    if (x == 0.0) return -math_bits::f64_of(0x7ff0000000000000ull);
    if (isinf(x)) return x;
    i32 e;
    f64 lm = log_reduce(x, &e);
    return (f64)e * kLog10_2 + lm * kInvLn10;
}

// C's table for the zeros, ones and infinities (Annex F) first.
f64 pow(f64 x, f64 y) {
    const f64 inf = math_bits::f64_of(0x7ff0000000000000ull);
    if (y == 0.0 || x == 1.0) return 1.0;
    if (isnan(x) || isnan(y)) return math_bits::nan64();
    bool odd = is_odd_integer(y);
    if (x == 0.0) {
        if (y < 0.0) return odd ? copysign(inf, x) : inf;
        return odd ? x : 0.0;
    }
    if (isinf(y)) {
        if (x == -1.0) return 1.0;
        return (fabs(x) < 1.0) == (y < 0.0) ? inf : 0.0;
    }
    if (isinf(x)) {
        bool neg = signbit(x) && odd;
        f64 r = y < 0.0 ? 0.0 : inf;
        return neg ? -r : r;
    }
    if (x < 0.0 && !is_integer(y)) return math_bits::nan64();
    f64 ax = fabs(x), r = 0.0;
    bool done = false;
    if (is_integer(y) && fabs(y) <= 64.0) {
        // Repeated squaring in two doubles: exact for an exact result, to about 2^-100 for
        // the rest. Past 2^±500 the split could overflow; the logarithm takes those.
        i64 n = (i64)fabs(y);
        dd b{ax, 0.0}, acc{1.0, 0.0};
        done = true;
        while (n) {
            if (n & 1) acc = mul(acc, b);
            n >>= 1;
            if (n) b = mul(b, b);
            f64 ah = fabs(acc.hi), bh = fabs(b.hi);
            if (ah > 0x1p500 || ah < 0x1p-500 || bh > 0x1p500 || bh < 0x1p-500) { done = false; break; }
        }
        if (done && y < 0.0) {
            // 1 / acc: q, then what q misses, from q * acc exactly.
            f64 q = 1.0 / acc.hi;
            dd qa = two_prod(q, acc.hi);
            r = q + q * (((1.0 - qa.hi) - qa.lo) - q * acc.lo);
        } else if (done) {
            r = acc.hi + acc.lo;
        }
    }
    if (done) {
    } else if (fabs(y) > 0x1p900) {
        // Unless x is 1 (above), |y log2 x| is past 2^840: an overflow or an underflow, and
        // y is too large to split.
        r = exp2(y * log2(ax));
    } else {
        dd l = log2_dd(ax);
        dd t = two_prod(y, l.hi);
        t.lo += y * l.lo;
        r = exp2_dd(fast_two_sum(t.hi, t.lo));
    }
    return (x < 0.0 && odd) ? -r : r;
}

f32 exp_hp(f32 x)   { return isnan(x) ? math_bits::nan32() : (f32)exp((f64)x); }
f32 exp2_hp(f32 x)  { return isnan(x) ? math_bits::nan32() : (f32)exp2((f64)x); }
f32 log_hp(f32 x)   { f64 r = log((f64)x); return isnan(r) ? math_bits::nan32() : (f32)r; }
f32 log2_hp(f32 x)  { f64 r = log2((f64)x); return isnan(r) ? math_bits::nan32() : (f32)r; }
f32 log10_hp(f32 x) { f64 r = log10((f64)x); return isnan(r) ? math_bits::nan32() : (f32)r; }
// The two-double pow, rounded: correctly rounded where the one-double fast path (fast.cc)
// misses about one input in 10^6.
f32 pow_hp(f32 x, f32 y) {
    f64 r = pow((f64)x, (f64)y);
    return isnan(r) ? math_bits::nan32() : (f32)r;
}

} // namespace kv
