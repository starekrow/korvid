#include "korvid/math/scalar.h"
#include "korvid/core/test.h"
#include <stdio.h>

// The scalar functions: C's special cases (Annex F), exact results that must be exact, and
// for each function a hash of every bit it returns over a few hundred thousand inputs.
// The hashes are the point: they were taken on Linux, and every platform must give the
// same ones. A platform that differs in one bit of one result prints the hash it got.

namespace {
using namespace kv;

u32 b32(f32 x) { return math_bits::of(x); }
u64 b64(f64 x) { return math_bits::of(x); }
bool same(f32 a, f32 b) { return b32(a) == b32(b); }
bool same(f64 a, f64 b) { return b64(a) == b64(b); }

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
    for (u64 i = 0; i < (1ull << 32); i += 16411) h.add(b32(f(math_bits::f32_of((u32)i))));
    return h.h;
}
// Pairs: random bits for one, a game's magnitudes for the other, then the reverse.
template <class F> u64 hash_f32_2(F f) {
    Hash h;
    u64 s = 0x2545f4914f6cdd1dull;
    for (i32 i = 0; i < 131072; i++) {
        f32 a = math_bits::f32_of((u32)xorshift(&s));
        f32 g = (f32)(i32)(u32)xorshift(&s) / 2147483648.0f * 16.0f;
        h.add(b32(f(a, g)));
        h.add(b32(f(g, a)));
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
    c.begin("scalar: special cases");
    const f32 inf = KV_INF, nan = KV_NAN;
    const f64 dinf = (f64)KV_INF;
    KV_CHECK(c, b32(nan) == 0x7fc00000u);
    KV_CHECK(c, same(sin(-0.0f), -0.0f) && same(tan(-0.0f), -0.0f) && same(atan(-0.0f), -0.0f) && same(asin(-0.0f), -0.0f));
    KV_CHECK(c, same(sin(inf), nan) && same(cos(-inf), nan) && same(tan(inf), nan) && same(sin(-nan), nan));
    KV_CHECK(c, same(atan2(0.0f, -0.0f), (f32)3.14159265358979f) && same(atan2(-0.0f, -0.0f), -(f32)3.14159265358979f));
    KV_CHECK(c, same(atan2(-0.0f, 1.0f), -0.0f) && same(atan2(1.0f, 0.0f), (f32)1.57079632679490f));
    KV_CHECK(c, same(atan2(inf, -inf), (f32)2.35619449019234f) && same(atan2(-inf, inf), -(f32)0.785398163397448f));
    KV_CHECK(c, same(atan2(1.0f, -inf), (f32)3.14159265358979f) && same(atan2(nan, 1.0f), nan));
    KV_CHECK(c, same(asin(1.5f), nan) && same(acos(-1.0f), (f32)3.14159265358979f) && same(acos(1.0f), 0.0f));
    KV_CHECK(c, same(exp(-inf), 0.0f) && same(exp(inf), inf) && same(exp(0.0f), 1.0f) && same(exp(-200.0f), 0.0f));
    KV_CHECK(c, same(log(0.0f), -inf) && same(log(-0.0f), -inf) && same(log(-1.0f), nan) && same(log(1.0f), 0.0f) && same(log(inf), inf));
    KV_CHECK(c, same(pow(nan, 0.0f), 1.0f) && same(pow(1.0f, nan), 1.0f) && same(pow(-1.0f, inf), 1.0f));
    KV_CHECK(c, same(pow(-0.0f, -3.0f), -inf) && same(pow(0.0f, -2.0f), inf) && same(pow(-0.0f, 3.0f), -0.0f) && same(pow(-0.0f, 2.0f), 0.0f));
    KV_CHECK(c, same(pow(-8.0f, 1.0f / 3.0f), nan) && same(pow(-2.0f, 3.0f), -8.0f) && same(pow(-inf, -3.0f), -0.0f));
    KV_CHECK(c, same(pow(0.5f, inf), 0.0f) && same(pow(0.5f, -inf), inf) && same(pow(2.0f, inf), inf) && same(pow(2.0f, -inf), 0.0f));
    KV_CHECK(c, same(sin(dinf), (f64)nan) && same(pow(-0.0, -1.0), -dinf) && same(atan2(-0.0, -1.0), -3.141592653589793));

    c.begin("scalar: exact results");
    KV_CHECK(c, pow(2.0f, 10.0f) == 1024.0f && pow(3.0f, 2.0f) == 9.0f && pow(10.0f, -2.0f) == 0.01f && pow(2.0f, 0.5f) == sqrt(2.0f));
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
    golden(c, "sin f32", hash_f32([](f32 x) { return sin(x); }), 0x944979969ce12289ull);
    golden(c, "cos f32", hash_f32([](f32 x) { return cos(x); }), 0xdcc94bc1c2b9729dull);
    golden(c, "tan f32", hash_f32([](f32 x) { return tan(x); }), 0xe44d235d18372006ull);
    golden(c, "asin f32", hash_f32([](f32 x) { return asin(x); }), 0x0cf61deebdf78cdcull);
    golden(c, "acos f32", hash_f32([](f32 x) { return acos(x); }), 0x13594601ed4448b5ull);
    golden(c, "atan f32", hash_f32([](f32 x) { return atan(x); }), 0x73416e5922b43228ull);
    golden(c, "exp f32", hash_f32([](f32 x) { return exp(x); }), 0x0e055cdd10973598ull);
    golden(c, "exp2 f32", hash_f32([](f32 x) { return exp2(x); }), 0x9b8edd21056a4486ull);
    golden(c, "log f32", hash_f32([](f32 x) { return log(x); }), 0xd31c40cdd791a12dull);
    golden(c, "log2 f32", hash_f32([](f32 x) { return log2(x); }), 0x868e965e105e5f79ull);
    golden(c, "log10 f32", hash_f32([](f32 x) { return log10(x); }), 0x54f001a55faddcfeull);
    golden(c, "atan2 f32", hash_f32_2([](f32 y, f32 x) { return atan2(y, x); }), 0xcb32a68b506347feull);
    golden(c, "pow f32", hash_f32_2([](f32 x, f32 y) { return pow(x, y); }), 0xfc926cb1c200918aull);
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
