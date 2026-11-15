#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_kimb(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_kimb(c);
    int rc = kv::test_report(c, "kimb_tests");
    kv::mem_shutdown();
    return rc;
}
