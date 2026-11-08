#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_writer(kv::TestCtx&);
void test_fields(kv::TestCtx&);
void test_kinds(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_writer(c);
    test_fields(c);
    test_kinds(c);
    kv::names_shutdown();
    kv::mem_shutdown();
    return kv::test_report(c, "save_tests");
}
