#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_path(kv::TestCtx&);
void test_stage(kv::TestCtx&);
void test_boss(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_path(c);
    test_stage(c);
    test_boss(c);
    int rc = kv::test_report(c, "example_arcfire_tests");
    kv::mem_shutdown();
    return rc;
}
