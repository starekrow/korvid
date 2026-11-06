#include "korvid/core/test.h"

void test_handle(kv::TestCtx&);
void test_event(kv::TestCtx&);
void test_tick(kv::TestCtx&);
void test_module(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_handle(c);
    test_event(c);
    test_tick(c);
    test_module(c);
    return kv::test_report(c, "core_tests");
}
