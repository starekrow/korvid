#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_zip(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_zip(c);
    int rc = kv::test_report(c, "pack_tests");
    kv::mem_shutdown();
    return rc;
}
