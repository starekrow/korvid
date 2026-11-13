// Korvid math: ldexp, frexp and fmod, by their bits. Each has one right answer, which this
// gives without the platform's help.
#include "korvid/math/scalar.h"

namespace kv {

// x * 2^n, in at most three steps of a power of two, so the result is rounded once: the
// last step alone can be inexact (into the subnormals), and the step before it keeps 53
// bits of room so that rounding is the only one.
f64 ldexp(f64 x, i32 n) {
    if (x == 0.0 || !isfinite(x)) return x;
    if (n > 1023) {
        x *= 0x1p1023;
        n -= 1023;
        if (n > 1023) { x *= 0x1p1023; n -= 1023; if (n > 1023) n = 1023; }
    } else if (n < -1022) {
        x *= 0x1p-1022 * 0x1p53;
        n += 1022 - 53;
        if (n < -1022) { x *= 0x1p-1022 * 0x1p53; n += 1022 - 53; if (n < -1022) n = -1022; }
    }
    return x * math_bits::f64_of((u64)(0x3ff + n) << 52);
}

f64 frexp(f64 x, i32* e) {
    *e = 0;
    if (x == 0.0 || !isfinite(x)) return x;
    u64 b = math_bits::of(x);
    i32 adj = 0;
    if (((b >> 52) & 0x7ff) == 0) { b = math_bits::of(x * 0x1p64); adj = -64; }
    *e = (i32)((b >> 52) & 0x7ff) - 1022 + adj;
    return math_bits::f64_of((b & 0x800fffffffffffffull) | 0x3fe0000000000000ull);
}
f32 frexp(f32 x, i32* e) { return (f32)frexp((f64)x, e); }

// Long division on the significands, one bit a step from x's exponent down to y's; the
// remainder never needs more than 54 bits.
f64 fmod(f64 x, f64 y) {
    if (isnan(x) || isnan(y) || isinf(x) || y == 0.0) return math_bits::nan64();
    u64 ux = math_bits::of(x), uy = math_bits::of(y);
    u64 sign = ux & 0x8000000000000000ull;
    u64 ax = ux & 0x7fffffffffffffffull, ay = uy & 0x7fffffffffffffffull;
    if (ax < ay) return x;          // |x| < |y|, y infinite included
    if (ax == ay) return math_bits::f64_of(sign);
    auto unpack = [](u64 a, u64* m, i32* e) {
        i32 ex = (i32)(a >> 52);
        u64 mm = a & 0xfffffffffffffull;
        if (ex == 0) { ex = 1; while (!(mm & (1ull << 52))) { mm <<= 1; ex--; } }
        else mm |= 1ull << 52;
        *m = mm;
        *e = ex;
    };
    u64 mx, my;
    i32 ex, ey;
    unpack(ax, &mx, &ex);
    unpack(ay, &my, &ey);
    for (; ex > ey; ex--) {
        if (mx >= my) { mx -= my; if (mx == 0) return math_bits::f64_of(sign); }
        mx <<= 1;
    }
    if (mx >= my) { mx -= my; if (mx == 0) return math_bits::f64_of(sign); }
    while (!(mx & (1ull << 52))) { mx <<= 1; ex--; }
    u64 out = ex > 0 ? ((u64)ex << 52) | (mx & 0xfffffffffffffull) : mx >> (1 - ex);
    return math_bits::f64_of(out | sign);
}

} // namespace kv
