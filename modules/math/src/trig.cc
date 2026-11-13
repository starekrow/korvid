// Korvid math: sin, cos and tan. x is brought to r in [-pi/4, pi/4] and a quadrant n with
// x = n*pi/2 + r: by Cody-Waite (pi/2 in three parts) below 2^20, by Payne-Hanek (2/pi's
// bits, as many as x's exponent needs) above. Then a series in r, in double. The f32
// functions are the f64 ones rounded once more.
#include "korvid/math/scalar.h"
#include "constants.h"

namespace kv {
namespace {
using namespace math_constants;

// sin r and cos r for |r| <= pi/4, to r^17 and r^16: what is left out is under 2^-55.
f64 sin_series(f64 r) {
    f64 z = r * r;
    f64 q = kInvFact[17];
    q = kInvFact[15] - z * q;
    q = kInvFact[13] - z * q;
    q = kInvFact[11] - z * q;
    q = kInvFact[9] - z * q;
    q = kInvFact[7] - z * q;
    q = kInvFact[5] - z * q;
    q = kInvFact[3] - z * q;
    return r - r * z * q;
}
f64 cos_series(f64 r) {
    f64 z = r * r;
    f64 q = kInvFact[16];
    q = kInvFact[14] - z * q;
    q = kInvFact[12] - z * q;
    q = kInvFact[10] - z * q;
    q = kInvFact[8] - z * q;
    q = kInvFact[6] - z * q;
    q = kInvFact[4] - z * q;
    q = kInvFact[2] - z * q;
    return 1.0 - z * q;
}

// 32 bits of 2/pi from bit pos (1: the first after the point).
u32 two_over_pi_bits(i32 pos) {
    i32 i = (pos - 1) >> 5, off = (pos - 1) & 31;
    u32 w = kTwoOverPi[i] << off;
    if (off) w |= kTwoOverPi[i + 1] >> (32 - off);
    return w;
}
// Bits [lo, lo+64) of a little-endian array of 32-bit limbs (zero past its end).
u64 limb_bits64(const u32* p, i32 n, i32 lo) {
    u64 v = 0;
    for (i32 b = 0; b < 64; b++) {
        i32 at = lo + b;
        if (at >= 0 && (at >> 5) < n && ((p[at >> 5] >> (at & 31)) & 1u)) v |= 1ull << b;
    }
    return v;
}

// Payne-Hanek for |x| >= 2^20 (finite). x = M * 2^e, M a 53-bit integer. Of x * 2/pi only
// the part mod 4 matters, so 2/pi's bits worth 4M or more are skipped: a 160-bit window
// from bit e - 1 on. M times it puts the units bit at bit q0; above that is the quadrant,
// below it the fraction, of which the top 63 bits from its leading one become r.
f64 reduce_large(f64 ax, i32* quadrant) {
    u64 b = math_bits::of(ax);
    i32 e = (i32)((b >> 52) & 0x7ff) - 1075;
    u64 m = (b & 0xfffffffffffffull) | (1ull << 52);
    i32 s = e - 1 < 1 ? 1 : e - 1;
    constexpr i32 W = 5; // limbs of the window: 160 bits
    u32 win[W];          // little-endian: win[0] the least significant
    for (i32 t = 0; t < W; t++) win[W - 1 - t] = two_over_pi_bits(s + 32 * t);
    u32 mm[2] = {(u32)m, (u32)(m >> 32)};
    u32 p[W + 2] = {};
    for (i32 i = 0; i < 2; i++) {
        u64 carry = 0;
        for (i32 j = 0; j < W; j++) {
            u64 t = (u64)mm[i] * win[j] + p[i + j] + carry;
            p[i + j] = (u32)t;
            carry = t >> 32;
        }
        p[i + W] = (u32)carry;
    }
    i32 q0 = s + 32 * W - 1 - e; // the bit worth 1
    i32 n = (i32)(limb_bits64(p, W + 2, q0) & 3);
    // The fraction: bits below q0. Past one half, it is taken from 1 and n goes up.
    u32 f[W + 2];
    for (i32 i = 0; i < W + 2; i++) {
        i32 lo = i * 32;
        u32 keep = lo + 32 <= q0 ? 0xffffffffu : (lo >= q0 ? 0u : (1u << (q0 - lo)) - 1u);
        f[i] = p[i] & keep;
    }
    bool neg = (limb_bits64(f, W + 2, q0 - 1) & 1) != 0;
    if (neg) {
        n = (n + 1) & 3;
        u64 carry = 1; // 2^q0 - f, as ~f + 1 within the field
        for (i32 i = 0; i < W + 2; i++) {
            i32 lo = i * 32;
            u32 keep = lo + 32 <= q0 ? 0xffffffffu : (lo >= q0 ? 0u : (1u << (q0 - lo)) - 1u);
            u64 t = (u64)(~f[i] & keep) + carry;
            f[i] = (u32)t & keep;
            carry = t >> 32;
        }
    }
    i32 top = -1;
    for (i32 i = W + 1; i >= 0 && top < 0; i--)
        if (f[i]) for (i32 j = 31; j >= 0; j--) if ((f[i] >> j) & 1u) { top = i * 32 + j; break; }
    *quadrant = n;
    if (top < 0) return 0.0;
    u64 lead = limb_bits64(f, W + 2, top - 62) & 0x7fffffffffffffffull; // 63 bits, the leading one first
    f64 fr = (f64)(i64)lead * math_bits::f64_of((u64)(1023 + top - 62 - q0) << 52);
    f64 r = fr * kPio2;
    return neg ? -r : r;
}

// r and the quadrant of finite x.
f64 reduce(f64 x, i32* quadrant) {
    f64 ax = fabs(x);
    if (ax < 1048576.0) {
        f64 k = math_bits::nearest(x * kInvPio2);
        *quadrant = (i32)((i64)k & 3);
        return ((x - k * kPio2_1) - k * kPio2_2) - k * kPio2_3;
    }
    i32 n;
    f64 r = reduce_large(ax, &n);
    if (x < 0.0) { r = -r; n = (4 - n) & 3; }
    *quadrant = n;
    return r;
}

// sin of n*pi/2 + r: sin r, cos r, -sin r, -cos r by the quadrant. Both series and a
// select by the bits, not a branch: the quadrant of an angle is as good as random.
f64 by_quadrant(f64 r, i32 n) {
    u64 s = math_bits::of(sin_series(r)), c = math_bits::of(cos_series(r));
    u64 odd = (u64)0 - (u64)(n & 1);
    return math_bits::f64_of(((s & ~odd) | (c & odd)) ^ ((u64)(n & 2) << 62));
}
} // namespace

// Below 2^-27 the cube term is under half an ulp: x itself, with its sign (-0 stays -0).
f64 sin(f64 x) {
    if (!isfinite(x)) return math_bits::nan64();
    if (fabs(x) < 0x1p-27) return x;
    i32 n;
    f64 r = reduce(x, &n);
    return by_quadrant(r, n);
}
f64 cos(f64 x) { // sin(x + pi/2)
    if (!isfinite(x)) return math_bits::nan64();
    i32 n;
    f64 r = reduce(x, &n);
    return by_quadrant(r, n + 1);
}
// s/c, or -c/s in an odd quadrant: the same select.
f64 tan(f64 x) {
    if (!isfinite(x)) return math_bits::nan64();
    if (fabs(x) < 0x1p-27) return x;
    i32 n;
    f64 r = reduce(x, &n);
    u64 s = math_bits::of(sin_series(r)), c = math_bits::of(cos_series(r));
    u64 odd = (u64)0 - (u64)(n & 1);
    f64 num = math_bits::f64_of((s & ~odd) | (c & odd)), den = math_bits::f64_of((c & ~odd) | (s & odd));
    return math_bits::f64_of(math_bits::of(num / den) ^ (odd & 0x8000000000000000ull));
}

f32 sin_hp(f32 x) { return isnan(x) ? math_bits::nan32() : (f32)sin((f64)x); }
f32 cos_hp(f32 x) { return isnan(x) ? math_bits::nan32() : (f32)cos((f64)x); }
f32 tan_hp(f32 x) { return isnan(x) ? math_bits::nan32() : (f32)tan((f64)x); }

} // namespace kv
