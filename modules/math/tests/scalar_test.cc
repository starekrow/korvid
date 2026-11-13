#include "korvid/math/scalar.h"
#include "korvid/core/test.h"
#include <stdio.h>

// The scalar functions: C's special cases (Annex F), exact results that must be exact, and
// for each function a hash of every bit it returns over a few hundred thousand inputs.
// The hashes are the point: they were taken on Linux, and every platform must give the
// same ones. A platform that differs in one bit of one result prints the hash it got. For
// the default f32 functions the hash is of their double results before the rounding to
// f32 (math_raw), since that rounding hides a difference in the arithmetic on nearly every
// input; the _hp f32 ones are their f64 functions rounded, and those are hashed as f64.

namespace {
using namespace kv;

u32 b32(f32 x) { return math_bits::of(x); }
u64 b64(f64 x) { return math_bits::of(x); }
bool same(f32 a, f32 b) { return b32(a) == b32(b); }
bool same(f64 a, f64 b) { return b64(a) == b64(b); }
u64 bits_any(f32 x) { return b32(x); }
u64 bits_any(f64 x) { return b64(x); }

struct Hash {
    u64 h = 0xcbf29ce484222325ull;
    void add(u64 v) {
        for (i32 i = 0; i < 8; i++) { h ^= (v >> (8 * i)) & 0xff; h *= 0x100000001b3ull; }
    }
};
u64 xorshift(u64* s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

// Every 16411th bit pattern of f32: all of the line, NaNs and subnormals included.
template <class F> u64 hash_f32(F f) {
    Hash h;
    for (u64 i = 0; i < (1ull << 32); i += 16411) h.add(bits_any(f(math_bits::f32_of((u32)i))));
    return h.h;
}
// Pairs: random bits for one, a game's magnitudes for the other, then the reverse.
template <class F> u64 hash_f32_2(F f) {
    Hash h;
    u64 s = 0x2545f4914f6cdd1dull;
    for (i32 i = 0; i < 131072; i++) {
        f32 a = math_bits::f32_of((u32)xorshift(&s));
        f32 g = (f32)(i32)(u32)xorshift(&s) / 2147483648.0f * 16.0f;
        h.add(bits_any(f(a, g)));
        h.add(bits_any(f(g, a)));
    }
    return h.h;
}
// Random f64 bit patterns, then a game's magnitudes.
template <class F> u64 hash_f64(F f) {
    Hash h;
    u64 s = 0x9e3779b97f4a7c15ull;
    for (i32 i = 0; i < 131072; i++) h.add(b64(f(math_bits::f64_of(xorshift(&s)))));
    for (i32 i = 0; i < 131072; i++) h.add(b64(f((f64)(i64)xorshift(&s) / 9223372036854775808.0 * 64.0)));
    return h.h;
}
template <class F> u64 hash_f64_2(F f) {
    Hash h;
    u64 s = 0xd1b54a32d192ed03ull;
    for (i32 i = 0; i < 131072; i++) {
        f64 a = math_bits::f64_of(xorshift(&s));
        f64 g = (f64)(i64)xorshift(&s) / 9223372036854775808.0 * 16.0;
        h.add(b64(f(a, g)));
        h.add(b64(f(g, a)));
    }
    return h.h;
}

void golden(TestCtx& c, const char* name, u64 got, u64 want) {
    if (got != want) fprintf(stderr, "math: %s hashes to 0x%016llxull, not 0x%016llxull\n", name, (unsigned long long)got, (unsigned long long)want);
    c.check(got == want, name, __FILE__, __LINE__);
}
} // namespace

void test_scalar(TestCtx& c) {
    const f32 inf = KV_INF, nan = KV_NAN;
    const f64 dinf = (f64)KV_INF;
    KV_CHECK(c, b32(nan) == 0x7fc00000u);
    // The defaults and the _hp functions answer C's special cases alike.
    struct Set {
        const char* name;
        f32 (*sin)(f32); f32 (*cos)(f32); f32 (*tan)(f32); f32 (*asin)(f32); f32 (*acos)(f32); f32 (*atan)(f32);
        f32 (*atan2)(f32, f32); f32 (*exp)(f32); f32 (*log)(f32); f32 (*pow)(f32, f32);
    };
    const Set sets[] = {
        {"scalar: special cases", sin, cos, tan, asin, acos, atan, atan2, exp, log, pow},
        {"scalar: special cases (_hp)", sin_hp, cos_hp, tan_hp, asin_hp, acos_hp, atan_hp, atan2_hp, exp_hp, log_hp, pow_hp},
    };
    for (const Set& f : sets) {
        c.begin(f.name);
        KV_CHECK(c, same(f.sin(-0.0f), -0.0f) && same(f.tan(-0.0f), -0.0f) && same(f.atan(-0.0f), -0.0f) && same(f.asin(-0.0f), -0.0f));
        KV_CHECK(c, same(f.sin(inf), nan) && same(f.cos(-inf), nan) && same(f.tan(inf), nan) && same(f.sin(-nan), nan) && same(f.cos(0.0f), 1.0f));
        KV_CHECK(c, same(f.atan2(0.0f, -0.0f), (f32)3.14159265358979f) && same(f.atan2(-0.0f, -0.0f), -(f32)3.14159265358979f));
        KV_CHECK(c, same(f.atan2(-0.0f, 1.0f), -0.0f) && same(f.atan2(1.0f, 0.0f), (f32)1.57079632679490f));
        KV_CHECK(c, same(f.atan2(inf, -inf), (f32)2.35619449019234f) && same(f.atan2(-inf, inf), -(f32)0.785398163397448f));
        KV_CHECK(c, same(f.atan2(1.0f, -inf), (f32)3.14159265358979f) && same(f.atan2(nan, 1.0f), nan) && same(f.atan(inf), (f32)1.57079632679490f));
        KV_CHECK(c, same(f.atan2(1.0f, 1.0f), (f32)0.785398163397448f) && same(f.atan2(-2.0f, -2.0f), -(f32)2.35619449019234f));
        KV_CHECK(c, same(f.asin(1.5f), nan) && same(f.acos(-1.0f), (f32)3.14159265358979f) && same(f.acos(1.0f), 0.0f) && same(f.asin(-1.0f), -(f32)1.57079632679490f));
        KV_CHECK(c, same(f.exp(-inf), 0.0f) && same(f.exp(inf), inf) && same(f.exp(0.0f), 1.0f) && same(f.exp(-200.0f), 0.0f) && same(f.exp(nan), nan));
        KV_CHECK(c, same(f.exp(88.8f), inf) && same(f.exp(-103.0f), 0x1p-149f) && same(f.atan(-inf), -(f32)1.57079632679490f));
        KV_CHECK(c, same(f.log(0.0f), -inf) && same(f.log(-0.0f), -inf) && same(f.log(-1.0f), nan) && same(f.log(1.0f), 0.0f) && same(f.log(inf), inf));
        KV_CHECK(c, same(f.log(nan), nan) && f.log(0x1p-149f) < -103.2f && f.log(0x1p-149f) > -103.3f);
        KV_CHECK(c, same(f.pow(nan, 0.0f), 1.0f) && same(f.pow(1.0f, nan), 1.0f) && same(f.pow(-1.0f, inf), 1.0f) && same(f.pow(1.0f, 7.5f), 1.0f));
        KV_CHECK(c, same(f.pow(-0.0f, -3.0f), -inf) && same(f.pow(0.0f, -2.0f), inf) && same(f.pow(-0.0f, 3.0f), -0.0f) && same(f.pow(-0.0f, 2.0f), 0.0f));
        KV_CHECK(c, same(f.pow(-8.0f, 1.0f / 3.0f), nan) && same(f.pow(-2.0f, 3.0f), -8.0f) && same(f.pow(-inf, -3.0f), -0.0f));
        KV_CHECK(c, same(f.pow(0.5f, inf), 0.0f) && same(f.pow(0.5f, -inf), inf) && same(f.pow(2.0f, inf), inf) && same(f.pow(2.0f, -inf), 0.0f));
        KV_CHECK(c, same(f.pow(2.0f, 200.0f), inf) && same(f.pow(2.0f, -200.0f), 0.0f) && same(f.pow(0x1p-149f, 0.5f), 0x1.6a09e6p-75f));
    }
    c.begin("scalar: f64 special cases");
    KV_CHECK(c, same(sin(dinf), (f64)nan) && same(pow(-0.0, -1.0), -dinf) && same(atan2(-0.0, -1.0), -3.141592653589793));

    c.begin("scalar: exact results");
    KV_CHECK(c, pow(2.0f, 10.0f) == 1024.0f && pow(3.0f, 2.0f) == 9.0f && pow(10.0f, -2.0f) == 0.01f && pow(2.0f, 0.5f) == sqrt(2.0f));
    KV_CHECK(c, pow_hp(2.0f, 10.0f) == 1024.0f && pow_hp(3.0f, 2.0f) == 9.0f && pow_hp(10.0f, -2.0f) == 0.01f && log2_hp(8.0f) == 3.0f);
    KV_CHECK(c, log2(0.25f) == -2.0f && exp2(5.0f) == 32.0f && exp2(-149.0f) == 0x1p-149f && log10(1000.0f) == 3.0f && exp(1.0f) == 2.71828182845904523536f);
    KV_CHECK(c, pow(3.0, 4.0) == 81.0 && pow(5.0, 3.0) == 125.0 && pow(7.0, 2.0) == 49.0); // stb_vorbis' codebook sizes
    KV_CHECK(c, log2(8.0f) == 3.0f && log2(0.125) == -3.0 && log2(1024.0) == 10.0 && exp2(10.0f) == 1024.0f && exp2(-3.0) == 0.125);
    KV_CHECK(c, sqrt(2.0f) == 1.41421353816986083984375f && same(sqrt(-0.0f), -0.0f) && isnan(sqrt(-1.0f)));
    KV_CHECK(c, same(floor(-0.5f), -1.0f) && same(floor(-0.0f), -0.0f) && same(ceil(-0.5f), -0.0f) && same(ceil(0.5f), 1.0f));
    KV_CHECK(c, same(round(0.49999997f), 0.0f) && same(round(-2.5f), -3.0f) && same(round(-0.25f), -0.0f) && lround(2.5f) == 3);
    KV_CHECK(c, same(trunc(-7.9), -7.0) && same(trunc(1e300), 1e300) && same(floor(8388609.0f), 8388609.0f) && isnan(floor(nan)));
    KV_CHECK(c, same(fmod(5.5f, 2.0f), 1.5f) && same(fmod(-5.5f, 2.0f), -1.5f) && same(fmod(-4.0f, 2.0f), -0.0f) && same(fmod(1.0f, inf), 1.0f));
    KV_CHECK(c, same(fmod(1e300, 7.0), 1.0) && isnan(fmod(1.0, 0.0)) && isnan(fmod(dinf, 1.0)));
    KV_CHECK(c, same(fmod(0x1p-1074, 0x1p-1073), 0x1p-1074) && same(fmod(0x1.8p-1070, 0x1p-1072), 0.0));
    KV_CHECK(c, ldexp(1.0, -1074) == 0x1p-1074 && ldexp(1.0, -1075) == 0.0 && ldexp(0x1.8p-1074, 0) == 0x1p-1073 && ldexp(1.0, 1024) == dinf);
    // Just over half the least subnormal, rounded once (up); rounding twice would give 0.
    KV_CHECK(c, ldexp(0x1.0000000000001p0, -1075) == 0x1p-1074 && ldexp(0x1p-1074, 2098) == dinf);
    i32 e;
    KV_CHECK(c, frexp(8.0, &e) == 0.5 && e == 4 && frexp(0x1p-1074, &e) == 0.5 && e == -1073 && frexp(-3.0f, &e) == -0.75f && e == 2);
    KV_CHECK(c, fabs(-0.0f) == 0.0f && !signbit(fabs(-0.0f)) && signbit(copysign(1.0f, -0.0f)) && isinf(-inf) && !isfinite(nan));

    c.begin("scalar: the same bits everywhere");
    golden(c, "sin f32 (before rounding)", hash_f32([](f32 x) { return math_raw::sin(x); }), 0xd369d4d01305656cull);
    golden(c, "sin_hp f32", hash_f32([](f32 x) { return sin_hp(x); }), 0x944979969ce12289ull);
    golden(c, "cos f32 (before rounding)", hash_f32([](f32 x) { return math_raw::cos(x); }), 0xbcbecda163137e43ull);
    golden(c, "cos_hp f32", hash_f32([](f32 x) { return cos_hp(x); }), 0xdcc94bc1c2b9729dull);
    golden(c, "tan f32 (before rounding)", hash_f32([](f32 x) { return math_raw::tan(x); }), 0x83e75c1b65466622ull);
    golden(c, "tan_hp f32", hash_f32([](f32 x) { return tan_hp(x); }), 0xe44d235d18372006ull);
    golden(c, "asin f32 (before rounding)", hash_f32([](f32 x) { return math_raw::asin(x); }), 0x3e9348a0872b0e6bull);
    golden(c, "asin_hp f32", hash_f32([](f32 x) { return asin_hp(x); }), 0x0cf61deebdf78cdcull);
    golden(c, "acos f32 (before rounding)", hash_f32([](f32 x) { return math_raw::acos(x); }), 0xeac170fa62c09220ull);
    golden(c, "acos_hp f32", hash_f32([](f32 x) { return acos_hp(x); }), 0x13594601ed4448b5ull);
    golden(c, "atan f32 (before rounding)", hash_f32([](f32 x) { return math_raw::atan(x); }), 0xd5cab2339f836e99ull);
    golden(c, "atan_hp f32", hash_f32([](f32 x) { return atan_hp(x); }), 0x73416e5922b43228ull);
    golden(c, "exp f32 (before rounding)", hash_f32([](f32 x) { return math_raw::exp(x); }), 0x0585d19641e8de44ull);
    golden(c, "exp_hp f32", hash_f32([](f32 x) { return exp_hp(x); }), 0x0e055cdd10973598ull);
    golden(c, "exp2 f32 (before rounding)", hash_f32([](f32 x) { return math_raw::exp2(x); }), 0x42cd862b5f9b8b73ull);
    golden(c, "exp2_hp f32", hash_f32([](f32 x) { return exp2_hp(x); }), 0x9b8edd21056a4486ull);
    golden(c, "log f32 (before rounding)", hash_f32([](f32 x) { return math_raw::log(x); }), 0xda526d90fd723340ull);
    golden(c, "log_hp f32", hash_f32([](f32 x) { return log_hp(x); }), 0xd31c40cdd791a12dull);
    golden(c, "log2 f32 (before rounding)", hash_f32([](f32 x) { return math_raw::log2(x); }), 0xc94b6276fc3406d3ull);
    golden(c, "log2_hp f32", hash_f32([](f32 x) { return log2_hp(x); }), 0x868e965e105e5f79ull);
    golden(c, "log10 f32 (before rounding)", hash_f32([](f32 x) { return math_raw::log10(x); }), 0xbc091fe7a2241019ull);
    golden(c, "log10_hp f32", hash_f32([](f32 x) { return log10_hp(x); }), 0x54f001a55faddcfeull);
    golden(c, "atan2 f32 (before rounding)", hash_f32_2([](f32 y, f32 x) { return math_raw::atan2(y, x); }), 0x3b89479283efebdaull);
    golden(c, "atan2_hp f32", hash_f32_2([](f32 y, f32 x) { return atan2_hp(y, x); }), 0xcb32a68b506347feull);
    golden(c, "pow f32 (before rounding)", hash_f32_2([](f32 x, f32 y) { return math_raw::pow(x, y); }), 0x680a23ed5744706eull);
    golden(c, "pow_hp f32", hash_f32_2([](f32 x, f32 y) { return pow_hp(x, y); }), 0xfc926cb1c200918aull);
    golden(c, "fmod f32", hash_f32_2([](f32 x, f32 y) { return fmod(x, y); }), 0x2fae630ec9407e01ull);
    golden(c, "sin f64", hash_f64([](f64 x) { return sin(x); }), 0x3ec043411fb3a99bull);
    golden(c, "cos f64", hash_f64([](f64 x) { return cos(x); }), 0xfe0fa95a89b7d1f0ull);
    golden(c, "tan f64", hash_f64([](f64 x) { return tan(x); }), 0x4b026b8be73c6038ull);
    golden(c, "asin f64", hash_f64([](f64 x) { return asin(x); }), 0x1450e80d2b50dc80ull);
    golden(c, "acos f64", hash_f64([](f64 x) { return acos(x); }), 0x55055ca9f79f4955ull);
    golden(c, "atan f64", hash_f64([](f64 x) { return atan(x); }), 0x8dba5275f6249ac0ull);
    golden(c, "exp f64", hash_f64([](f64 x) { return exp(x); }), 0x0ee30212078b59dfull);
    golden(c, "exp2 f64", hash_f64([](f64 x) { return exp2(x); }), 0xedf7eca410e469acull);
    golden(c, "log f64", hash_f64([](f64 x) { return log(x); }), 0x7eb78e5390089f54ull);
    golden(c, "log2 f64", hash_f64([](f64 x) { return log2(x); }), 0x4c180cfd9598f025ull);
    golden(c, "log10 f64", hash_f64([](f64 x) { return log10(x); }), 0xd8049d9582b4f420ull);
    golden(c, "atan2 f64", hash_f64_2([](f64 y, f64 x) { return atan2(y, x); }), 0x2e7ecca839f387b2ull);
    golden(c, "pow f64", hash_f64_2([](f64 x, f64 y) { return pow(x, y); }), 0xc53835317609e54full);
    golden(c, "fmod f64", hash_f64_2([](f64 x, f64 y) { return fmod(x, y); }), 0x7f15846d92a9994dull);
    golden(c, "floor/round f64", hash_f64([](f64 x) { return floor(x) + 0.5 * round(x) + 0.25 * trunc(x) + ceil(x); }), 0xf3d2b73630b3bfe7ull);
}
