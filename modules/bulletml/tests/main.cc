#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_bulletml(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_bulletml(c);
    int rc = kv::test_report(c, "bulletml_tests");
    kv::mem_shutdown();
    return rc;
}
