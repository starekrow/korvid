#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_typeset(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_typeset(c);
    int rc = kv::test_report(c, "typeset_tests");
    kv::mem_shutdown();
    return rc;
}
