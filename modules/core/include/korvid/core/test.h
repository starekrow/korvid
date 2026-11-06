// Korvid core: a tiny test harness. One test executable per module; each test file
// exports a void fn(TestCtx&) that tests/main.cc calls explicitly. No registration magic.
#pragma once
#include "korvid/core/log.h"

namespace kv {

struct TestCtx {
    u32         checks   = 0;
    u32         failures = 0;
    const char* suite    = "";

    void begin(const char* name) { suite = name; }
    void check(bool ok, const char* expr, const char* file, int line) {
        ++checks;
        if (!ok) {
            ++failures;
            KV_ERROR("test", "%s: FAIL %s (%s:%d)", suite, expr, file, line);
        }
    }
};

inline int test_report(const TestCtx& c, const char* exe) {
    if (c.failures) KV_ERROR("test", "%s: %u of %u checks failed", exe, c.failures, c.checks);
    else            KV_INFO ("test", "%s: %u checks passed", exe, c.checks);
    return c.failures ? 1 : 0;
}

} // namespace kv

// Variadic so a braced initializer with commas works inside the expression.
#define KV_CHECK(ctx, ...) (ctx).check((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
