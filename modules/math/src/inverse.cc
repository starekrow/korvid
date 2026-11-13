// Korvid math: atan, atan2, asin and acos. atan of t in [0, 1] is atan(c) for the nearest
// c = k/8, from a table, plus atan of what is left, u = (t - c) / (1 + t*c), |u| <= 1/16,
// as a series. Larger t is pi/2 less atan(1/t); asin and acos are atan2 of sqrt(1 - x^2)
// and x. The f32 functions are the f64 ones rounded once more.
#include "korvid/math/scalar.h"
#include "constants.h"

namespace kv {
namespace {
using namespace math_constants;

f64 atan01(f64 t) {
    i32 k = (i32)(t * 8.0 + 0.5);
    f64 c = (f64)k * 0.125;
    f64 u = (t - c) / (1.0 + t * c);
    f64 z = u * u;
    // u - u^3/3 + ... - u^15/15: the next term is under 2^-60 of u.
    f64 q = kInvOdd[7];
    q = kInvOdd[6] - z * q;
    q = kInvOdd[5] - z * q;
    q = kInvOdd[4] - z * q;
    q = kInvOdd[3] - z * q;
    q = kInvOdd[2] - z * q;
    q = kInvOdd[1] - z * q;
    return kAtanHi[k] + (kAtanLo[k] + (u - u * z * q));
}
} // namespace

f64 atan(f64 x) {
    if (isnan(x)) return math_bits::nan64();
    f64 ax = fabs(x);
    if (ax < 0x1p-27) return x;
    f64 a = ax <= 1.0 ? atan01(ax) : (kPio2 - atan01(1.0 / ax)) + kPio2Lo;
    return copysign(a, x);
}

// C's table for the zeros and infinities (Annex F); otherwise the angle of the smaller
// over the larger, put in its octant.
f64 atan2(f64 y, f64 x) {
    if (isnan(x) || isnan(y)) return math_bits::nan64();
    f64 ay = fabs(y), ax = fabs(x);
    f64 a;
    if (ay == 0.0) a = signbit(x) ? kPi : 0.0;
    else if (ax == 0.0) a = kPio2;
    else if (isinf(ay)) a = isinf(ax) ? (signbit(x) ? k3Pio4 : kPio4) : kPio2;
    else if (isinf(ax)) a = signbit(x) ? kPi : 0.0;
    else if (ay <= ax) {
        a = atan01(ay / ax);
        if (signbit(x)) a = (kPi - a) + kPiLo;
    } else {
        f64 b = atan01(ax / ay);
        a = signbit(x) ? (kPio2 + b) + kPio2Lo : (kPio2 - b) + kPio2Lo;
    }
    return copysign(a, y);
}

// (1 - x)(1 + x): 1 - x is exact where it matters, near |x| = 1.
f64 asin(f64 x) {
    if (isnan(x) || fabs(x) > 1.0) return math_bits::nan64();
    return atan2(x, sqrt((1.0 - x) * (1.0 + x)));
}
f64 acos(f64 x) {
    if (isnan(x) || fabs(x) > 1.0) return math_bits::nan64();
    return atan2(sqrt((1.0 - x) * (1.0 + x)), x);
}

f32 atan(f32 x) { return isnan(x) ? math_bits::nan32() : (f32)atan((f64)x); }
f32 atan2(f32 y, f32 x) { return isnan(x) || isnan(y) ? math_bits::nan32() : (f32)atan2((f64)y, (f64)x); }
f32 asin(f32 x) { return isnan(x) || fabs(x) > 1.0f ? math_bits::nan32() : (f32)asin((f64)x); }
f32 acos(f32 x) { return isnan(x) || fabs(x) > 1.0f ? math_bits::nan32() : (f32)acos((f64)x); }

} // namespace kv
