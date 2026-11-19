// Korvid core: the test harness: TestCtx counts checks, KV_CHECK makes one, test_report
// turns the count into the exit code.
//
// Each module has one test executable, bin/<module>_tests, built from every .cc file in
// its tests/ directory. `kormake test core` builds core's and runs it; it passes when it
// exits 0, and kormake does not run it again until it changes. There is no registration:
// each test file defines a function taking a TestCtx&, and tests/main.cc declares each
// one and calls it, then returns test_report's result (modules/core/tests):
//
//   // random_test.cc
//   void test_random(kv::TestCtx& c) {
//       using namespace kv;
//       c.begin("random");
//       Rng ref = Rng::seeded(42, 54);
//       KV_CHECK(c, ref.next() == 0xa15c02b7u);   // PCG32's published first output
//   }
//
//   // main.cc
//   void test_random(kv::TestCtx&);
//   int main() {
//       kv::TestCtx c;
//       test_random(c);
//       kv::names_shutdown();
//       kv::mem_shutdown(); // the leak report, if anything is left
//       return kv::test_report(c, "core_tests");
//   }
//
// A failed check logs an error and the run carries on, so one run lists every failure:
//
//   [ERROR] test: random: FAIL ref.next() == 0xa15c02b7u (.../core/tests/random_test.cc:21)
#pragma once
#include "korvid/core/log.h"

namespace kv {

// One test run's tally, passed to every test function.
struct TestCtx {
    u32         checks   = 0;  // checks made
    u32         failures = 0;  // of those, the ones that failed
    const char* suite    = ""; // the name begin() set, at the head of each failure line

    // Names the checks that follow in the failure lines: once at the top of each test
    // function, or of each part of one.
    void begin(const char* name) { suite = name; }
    // Counts one check and logs it if `ok` is false. KV_CHECK calls it with the
    // expression's text, file and line.
    void check(bool ok, const char* expr, const char* file, int line) {
        ++checks;
        if (!ok) {
            ++failures;
            KV_ERROR("test", "%s: FAIL %s (%s:%d)", suite, expr, file, line);
        }
    }
};

// Logs the totals under `exe`, the executable's name, and returns the exit code: 1 if any
// check failed, 0 if none did. main's last line.
inline int test_report(const TestCtx& c, const char* exe) {
    if (c.failures) KV_ERROR("test", "%s: %u of %u checks failed", exe, c.failures, c.checks);
    else            KV_INFO ("test", "%s: %u checks passed", exe, c.checks);
    return c.failures ? 1 : 0;
}

} // namespace kv

// Checks one condition and carries on: KV_CHECK(c, list.len == 3). Variadic so a braced
// initializer with commas works inside the expression.
#define KV_CHECK(ctx, ...) (ctx).check((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
