// Korvid math's speed against glibc's: ns per call over game-sized inputs, each function
// on the same 4M inputs, results summed so nothing is optimized away. One thread. bench.sh
// builds it with the engine's flags.
#include "korvid/math/scalar.h"
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <vector>

using kv::f32;
using kv::f64;
using kv::u64;

static double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

template <class T, class F> static double run(const std::vector<T>& in, F f, double* sink) {
    double best = 1e9;
    for (int rep = 0; rep < 5; rep++) {
        double t0 = now(), s = 0;
        for (T x : in) s += (double)f(x);
        double t = (now() - t0) / (double)in.size() * 1e9;
        if (t < best) best = t;
        *sink += s;
    }
    return best;
}

int main() {
    const int N = 1 << 22;
    std::vector<f32> angle(N), pos(N), unit(N), wide(N);
    std::vector<f64> dangle(N), dpos(N);
    u64 s = 0x9e3779b97f4a7c15ull;
    auto next = [&] { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (double)(s >> 11) / 9007199254740992.0; };
    for (int i = 0; i < N; i++) {
        angle[i] = (f32)((next() - 0.5) * 20.0);   // radians a game turns by
        pos[i] = (f32)(next() * 4.0);              // a colour channel, a distance
        unit[i] = (f32)(next() * 2.0 - 1.0);
        wide[i] = (f32)((next() - 0.5) * 200.0);
        dangle[i] = (next() - 0.5) * 20.0;
        dpos[i] = next() * 100.0;
    }
    double sink = 0;
    struct Row { const char* name; double kv, glibc; };
    std::vector<Row> rows;
    rows.push_back({"sinf", run(angle, [](f32 x) { return kv::sin(x); }, &sink), run(angle, [](f32 x) { return ::sinf(x); }, &sink)});
    rows.push_back({"cosf", run(angle, [](f32 x) { return kv::cos(x); }, &sink), run(angle, [](f32 x) { return ::cosf(x); }, &sink)});
    rows.push_back({"tanf", run(angle, [](f32 x) { return kv::tan(x); }, &sink), run(angle, [](f32 x) { return ::tanf(x); }, &sink)});
    rows.push_back({"atanf", run(wide, [](f32 x) { return kv::atan(x); }, &sink), run(wide, [](f32 x) { return ::atanf(x); }, &sink)});
    rows.push_back({"atan2f", run(angle, [&](f32 x) { return kv::atan2(x, 1.5f - x * 0.25f); }, &sink), run(angle, [&](f32 x) { return ::atan2f(x, 1.5f - x * 0.25f); }, &sink)});
    rows.push_back({"asinf", run(unit, [](f32 x) { return kv::asin(x); }, &sink), run(unit, [](f32 x) { return ::asinf(x); }, &sink)});
    rows.push_back({"expf", run(angle, [](f32 x) { return kv::exp(x); }, &sink), run(angle, [](f32 x) { return ::expf(x); }, &sink)});
    rows.push_back({"logf", run(pos, [](f32 x) { return kv::log(x); }, &sink), run(pos, [](f32 x) { return ::logf(x); }, &sink)});
    rows.push_back({"powf 2.4", run(pos, [](f32 x) { return kv::pow(x, 2.4f); }, &sink), run(pos, [](f32 x) { return ::powf(x, 2.4f); }, &sink)});
    rows.push_back({"sqrtf", run(pos, [](f32 x) { return kv::sqrt(x); }, &sink), run(pos, [](f32 x) { return ::sqrtf(x); }, &sink)});
    rows.push_back({"floorf", run(wide, [](f32 x) { return kv::floor(x); }, &sink), run(wide, [](f32 x) { return ::floorf(x); }, &sink)});
    rows.push_back({"fmodf", run(wide, [](f32 x) { return kv::fmod(x, 6.25f); }, &sink), run(wide, [](f32 x) { return ::fmodf(x, 6.25f); }, &sink)});
    rows.push_back({"sin", run(dangle, [](f64 x) { return kv::sin(x); }, &sink), run(dangle, [](f64 x) { return ::sin(x); }, &sink)});
    rows.push_back({"exp", run(dangle, [](f64 x) { return kv::exp(x); }, &sink), run(dangle, [](f64 x) { return ::exp(x); }, &sink)});
    rows.push_back({"log", run(dpos, [](f64 x) { return kv::log(x); }, &sink), run(dpos, [](f64 x) { return ::log(x); }, &sink)});
    rows.push_back({"pow", run(dpos, [](f64 x) { return kv::pow(x, 0.75); }, &sink), run(dpos, [](f64 x) { return ::pow(x, 0.75); }, &sink)});
    printf("%-10s %8s %8s %7s\n", "function", "kv ns", "glibc ns", "ratio");
    for (const Row& r : rows) printf("%-10s %8.2f %8.2f %6.2fx\n", r.name, r.kv, r.glibc, r.kv / r.glibc);
    printf("(sink %g)\n", sink);
    return 0;
}
