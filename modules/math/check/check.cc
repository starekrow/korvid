// Korvid math's accuracy check: every f32 input of each one-argument function, and samples
// of the two-argument ones and of the f64 functions, against MPFR (correctly rounded at
// 160 bits, then to the type). Linux, with the libmpfr GCC already brings; run.sh builds
// it. MPFR's header is not needed: the few functions used are declared here.
//
// For f32 the reference is glibc's double function rounded to float, which is the right
// answer unless the double lands within a few of its ulps of a float rounding boundary
// (glibc's double functions are within an ulp); there, and wherever kv disagrees, MPFR
// decides. So a pass over all 2^32 floats takes seconds, not hours.
#include "korvid/math/scalar.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <atomic>
#include <thread>
#include <vector>

using kv::i32;
using kv::i64;
using kv::u32;
using kv::u64;

extern "C" {
typedef struct { long prec; int sign; long exp; void* d; } mpfr_struct;
void mpfr_init2(mpfr_struct*, long);
void mpfr_clear(mpfr_struct*);
int mpfr_set_d(mpfr_struct*, double, int);
double mpfr_get_d(const mpfr_struct*, int);
float mpfr_get_flt(const mpfr_struct*, int);
int mpfr_sin(mpfr_struct*, const mpfr_struct*, int);
int mpfr_cos(mpfr_struct*, const mpfr_struct*, int);
int mpfr_tan(mpfr_struct*, const mpfr_struct*, int);
int mpfr_asin(mpfr_struct*, const mpfr_struct*, int);
int mpfr_acos(mpfr_struct*, const mpfr_struct*, int);
int mpfr_atan(mpfr_struct*, const mpfr_struct*, int);
int mpfr_exp(mpfr_struct*, const mpfr_struct*, int);
int mpfr_exp2(mpfr_struct*, const mpfr_struct*, int);
int mpfr_log(mpfr_struct*, const mpfr_struct*, int);
int mpfr_log2(mpfr_struct*, const mpfr_struct*, int);
int mpfr_log10(mpfr_struct*, const mpfr_struct*, int);
int mpfr_atan2(mpfr_struct*, const mpfr_struct*, const mpfr_struct*, int);
int mpfr_pow(mpfr_struct*, const mpfr_struct*, const mpfr_struct*, int);
}

typedef int (*MpfrFn1)(mpfr_struct*, const mpfr_struct*, int);
typedef int (*MpfrFn2)(mpfr_struct*, const mpfr_struct*, const mpfr_struct*, int);

struct Fn1 {
    const char* name;
    float (*kv32)(float);
    double (*kv64)(double);
    double (*glibc)(double);
    MpfrFn1 mpfr;
};
struct Fn2 {
    const char* name;
    float (*kv32)(float, float);
    double (*kv64)(double, double);
    MpfrFn2 mpfr;
};

// The defaults, then the _hp ones; the f64 functions once (with the defaults' rows).
static const Fn1 kFn1[] = {
    {"sin", kv::sin, kv::sin, ::sin, mpfr_sin},       {"cos", kv::cos, kv::cos, ::cos, mpfr_cos},
    {"tan", kv::tan, kv::tan, ::tan, mpfr_tan},       {"asin", kv::asin, kv::asin, ::asin, mpfr_asin},
    {"acos", kv::acos, kv::acos, ::acos, mpfr_acos},  {"atan", kv::atan, kv::atan, ::atan, mpfr_atan},
    {"exp", kv::exp, kv::exp, ::exp, mpfr_exp},       {"exp2", kv::exp2, kv::exp2, ::exp2, mpfr_exp2},
    {"log", kv::log, kv::log, ::log, mpfr_log},       {"log2", kv::log2, kv::log2, ::log2, mpfr_log2},
    {"log10", kv::log10, kv::log10, ::log10, mpfr_log10},
    {"sin_hp", kv::sin_hp, nullptr, ::sin, mpfr_sin},       {"cos_hp", kv::cos_hp, nullptr, ::cos, mpfr_cos},
    {"tan_hp", kv::tan_hp, nullptr, ::tan, mpfr_tan},       {"asin_hp", kv::asin_hp, nullptr, ::asin, mpfr_asin},
    {"acos_hp", kv::acos_hp, nullptr, ::acos, mpfr_acos},  {"atan_hp", kv::atan_hp, nullptr, ::atan, mpfr_atan},
    {"exp_hp", kv::exp_hp, nullptr, ::exp, mpfr_exp},       {"exp2_hp", kv::exp2_hp, nullptr, ::exp2, mpfr_exp2},
    {"log_hp", kv::log_hp, nullptr, ::log, mpfr_log},       {"log2_hp", kv::log2_hp, nullptr, ::log2, mpfr_log2},
    {"log10_hp", kv::log10_hp, nullptr, ::log10, mpfr_log10},
};
static const Fn2 kFn2[] = {
    {"atan2", kv::atan2, kv::atan2, mpfr_atan2},
    {"pow", kv::pow, kv::pow, mpfr_pow},
    {"atan2_hp", kv::atan2_hp, nullptr, mpfr_atan2},
    {"pow_hp", kv::pow_hp, nullptr, mpfr_pow},
};

static u32 bits32(float x) { u32 b; memcpy(&b, &x, 4); return b; }
static u64 bits64(double x) { u64 b; memcpy(&b, &x, 8); return b; }
// Distance in units in the last place, NaN against NaN being 0.
static u64 ulps32(float a, float b) {
    if (isnan(a) || isnan(b)) return isnan(a) && isnan(b) ? 0 : ~0ull;
    i64 ia = (i32)bits32(a), ib = (i32)bits32(b);
    if (ia < 0) ia = (i64)INT32_MIN - ia;
    if (ib < 0) ib = (i64)INT32_MIN - ib;
    return (u64)(ia > ib ? ia - ib : ib - ia);
}
static u64 ulps64(double a, double b) {
    if (isnan(a) || isnan(b)) return isnan(a) && isnan(b) ? 0 : ~0ull;
    __int128 ia = (i64)bits64(a), ib = (i64)bits64(b);
    if (ia < 0) ia = (__int128)INT64_MIN - ia;
    if (ib < 0) ib = (__int128)INT64_MIN - ib;
    __int128 d = ia > ib ? ia - ib : ib - ia;
    return d > (__int128)~0ull ? ~0ull : (u64)d;
}

struct Mpfr {
    mpfr_struct a, b, r;
    Mpfr() { mpfr_init2(&a, 160); mpfr_init2(&b, 160); mpfr_init2(&r, 160); }
    ~Mpfr() { mpfr_clear(&a); mpfr_clear(&b); mpfr_clear(&r); }
    float f32(MpfrFn1 f, float x) { mpfr_set_d(&a, x, 0); f(&r, &a, 0); return mpfr_get_flt(&r, 0); }
    double f64(MpfrFn1 f, double x) { mpfr_set_d(&a, x, 0); f(&r, &a, 0); return mpfr_get_d(&r, 0); }
    float f32(MpfrFn2 f, float x, float y) { mpfr_set_d(&a, x, 0); mpfr_set_d(&b, y, 0); f(&r, &a, &b, 0); return mpfr_get_flt(&r, 0); }
    double f64(MpfrFn2 f, double x, double y) { mpfr_set_d(&a, x, 0); mpfr_set_d(&b, y, 0); f(&r, &a, &b, 0); return mpfr_get_d(&r, 0); }
};

// Whether d, rounded to float, could round the other way were it a few ulps off.
static bool near_boundary(double d) {
    if (isnan(d) || isinf(d) || d == 0.0) return false;
    float f = (float)d;
    if (isinf(f)) return true;
    float g = nextafterf(f, d > (double)f ? INFINITY : -INFINITY);
    if (isinf(g)) return true;
    double mid = ((double)f + (double)g) * 0.5;
    double gap = fabs(d - mid);
    return gap <= 8.0 * (nextafter(fabs(d), INFINITY) - fabs(d));
}

struct Tally {
    std::atomic<u64> wrong{0}, mpfr_calls{0}, worst{0}, worst_bits{0}, worst_bits2{0};
    std::atomic<u64> nan_bad{0};
    void note(u64 u, u64 input, u64 input2 = 0) {
        if (!u) return;
        wrong++;
        u64 w = worst.load();
        while (u > w && !worst.compare_exchange_weak(w, u)) {}
        if (u >= w) { worst_bits = input; worst_bits2 = input2; }
    }
};

static i32 g_threads = 16;

static void exhaustive_f32(const Fn1& fn) {
    Tally t;
    std::vector<std::thread> th;
    for (i32 k = 0; k < g_threads; k++) {
        th.emplace_back([&, k] {
            Mpfr m;
            for (u64 i = (u64)k; i < (1ull << 32); i += (u64)g_threads) {
                float x;
                u32 b = (u32)i;
                memcpy(&x, &b, 4);
                float got = fn.kv32(x);
                if (isnan(got) && bits32(got) != 0x7fc00000u) t.nan_bad++;
                double d = fn.glibc((double)x);
                float want = (float)d;
                if (bits32(got) != bits32(want) || near_boundary(d)) {
                    if (!(isnan(got) && isnan(want))) {
                        want = m.f32(fn.mpfr, x);
                        t.mpfr_calls++;
                    }
                }
                t.note(ulps32(got, want), b);
            }
        });
    }
    for (auto& x : th) x.join();
    float wx;
    u32 wb = (u32)t.worst_bits.load();
    memcpy(&wx, &wb, 4);
    printf("%-6s f32: all 2^32 inputs: %llu not correctly rounded (%.2g%%), worst %llu ulp; MPFR asked %llu times%s\n", fn.name,
           (unsigned long long)t.wrong.load(), 100.0 * (double)t.wrong.load() / 4294967296.0, (unsigned long long)t.worst.load(),
           (unsigned long long)t.mpfr_calls.load(), t.nan_bad.load() ? " [NON-CANONICAL NaN]" : "");
    if (t.worst.load()) printf("         worst input %a\n", (double)wx);
}

static u64 rng_next(u64* s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

// Inputs a game makes (small and moderate magnitudes) and anything at all (random bits).
static float sample32(u64* s, i32 i) {
    u32 b = (u32)rng_next(s);
    float x;
    if (i & 1) { memcpy(&x, &b, 4); return x; }
    return (float)((i32)b) / 2147483648.0f * 64.0f;
}
static double sample64(u64* s, i32 i) {
    u64 b = rng_next(s);
    double x;
    if (i & 1) { memcpy(&x, &b, 8); return x; }
    return (double)(i64)b / 9223372036854775808.0 * 64.0;
}

static void sampled_f32_2(const Fn2& fn, i64 n) {
    Tally t;
    std::vector<std::thread> th;
    for (i32 k = 0; k < g_threads; k++) {
        th.emplace_back([&, k] {
            Mpfr m;
            u64 s = 0x9e3779b97f4a7c15ull * (u64)(k + 1);
            for (i64 i = k; i < n; i += g_threads) {
                float x = sample32(&s, (i32)i), y = sample32(&s, (i32)(i >> 1));
                if (fn.mpfr == mpfr_pow && (i % 3 == 0)) { x = fabsf(x); y = (float)(i % 7) - 3.0f + (i % 5 == 0 ? 0.4f : 0.0f); }
                float got = fn.kv32(x, y);
                t.note(ulps32(got, m.f32(fn.mpfr, x, y)), bits32(x));
            }
        });
    }
    for (auto& x : th) x.join();
    printf("%-6s f32: %lld sampled pairs: %llu not correctly rounded (%.2g%%), worst %llu ulp\n", fn.name, (long long)n,
           (unsigned long long)t.wrong.load(), 100.0 * (double)t.wrong.load() / (double)n, (unsigned long long)t.worst.load());
}

template <class F> static void sampled_f64(const char* name, F f, i64 n) {
    Tally t;
    std::vector<std::thread> th;
    for (i32 k = 0; k < g_threads; k++) {
        th.emplace_back([&, k] {
            Mpfr m;
            u64 s = 0xd1b54a32d192ed03ull * (u64)(k + 1);
            for (i64 i = k; i < n; i += g_threads) {
                double x = sample64(&s, (i32)i), y = sample64(&s, (i32)(i >> 1));
                double got, want;
                f(m, x, y, &got, &want);
                t.note(ulps64(got, want), bits64(x), bits64(y));
            }
        });
    }
    for (auto& x : th) x.join();
    printf("%-6s f64: %lld samples: %llu not correctly rounded (%.2g%%), worst %llu ulp\n", name, (long long)n,
           (unsigned long long)t.wrong.load(), 100.0 * (double)t.wrong.load() / (double)n, (unsigned long long)t.worst.load());
    if (t.worst.load()) {
        u64 a = t.worst_bits.load(), b = t.worst_bits2.load();
        double x, y;
        memcpy(&x, &a, 8);
        memcpy(&y, &b, 8);
        printf("         worst input %a (second argument, if any: %a)\n", x, y);
    }
}

int main(int argc, char** argv) {
    const char* only = argc > 1 ? argv[1] : nullptr;
    g_threads = (i32)std::thread::hardware_concurrency();
    if (g_threads < 1) g_threads = 1;
    for (const Fn1& f : kFn1) if (!only || !strcmp(only, f.name)) exhaustive_f32(f);
    for (const Fn2& f : kFn2) if (!only || !strcmp(only, f.name)) sampled_f32_2(f, 200000000);
    for (const Fn1& f : kFn1) {
        if ((only && strcmp(only, f.name)) || !f.kv64) continue;
        sampled_f64(f.name, [&](Mpfr& m, double x, double, double* got, double* want) { *got = f.kv64(x); *want = m.f64(f.mpfr, x); }, 4000000);
    }
    for (const Fn2& f : kFn2) {
        if ((only && strcmp(only, f.name)) || !f.kv64) continue;
        sampled_f64(f.name, [&](Mpfr& m, double x, double y, double* got, double* want) {
            if (f.mpfr == mpfr_pow) { x = fabs(x); }
            *got = f.kv64(x, y); *want = m.f64(f.mpfr, x, y); }, 4000000);
    }
    return 0;
}
