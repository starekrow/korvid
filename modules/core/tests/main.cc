#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_handle(kv::TestCtx&);
void test_event(kv::TestCtx&);
void test_tick(kv::TestCtx&);
void test_module(kv::TestCtx&);
void test_memory(kv::TestCtx&);
void test_map(kv::TestCtx&);
void test_name(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_memory(c);
    test_handle(c);
    test_event(c);
    test_tick(c);
    test_module(c);
    test_map(c);
    test_name(c);
    kv::names_shutdown();
    kv::mem_shutdown(); // the leak report, if anything is left
    return kv::test_report(c, "core_tests");
}
