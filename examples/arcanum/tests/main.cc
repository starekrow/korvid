#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_credits(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_credits(c);
    int rc = kv::test_report(c, "example_arcanum_tests");
    kv::mem_shutdown();
    return rc;
}
