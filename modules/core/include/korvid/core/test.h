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
//
// A test that reads a file of the source tree (a font, a reference picture) names it by
// its path from the repository's root, through test_source_path, and its module lists it
// in module.lua's test_files, so a device that has no source tree is given a copy.
#pragma once
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include <string.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
#include <mach-o/dyld.h>
#define KV_TEST_SOURCE_BUNDLED 1
#endif
#endif

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

// The path of `rel`, a file or directory of the source tree by its path from the root
// ("modules/dejavu/assets/DejaVuSans.ttf"). The root is where this header is: the build
// names headers by their full paths. An iOS device has no source tree: the test runner
// copies what the module's test_files lists into source/ beside the executable, and the
// path is there.
inline void test_source_path(char* out, usize cap, const char* rel) {
#if defined(KV_TEST_SOURCE_BUNDLED)
    char exe[1024];
    uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n) != 0) exe[0] = 0;
    if (char* slash = strrchr(exe, '/')) *slash = 0;
    format(out, cap, "%s/source/%s", exe, rel);
#else
    char root[1024];
    format(root, sizeof root, "%s", __FILE__); // <root>/modules/core/include/korvid/core/test.h
    for (int up = 0; up < 6; ++up) if (char* slash = strrchr(root, '/')) *slash = 0;
    format(out, cap, "%s/%s", root, rel);
#endif
}

} // namespace kv

// Checks one condition and carries on: KV_CHECK(c, list.len == 3). Variadic so a braced
// initializer with commas works inside the expression.
#define KV_CHECK(ctx, ...) (ctx).check((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
