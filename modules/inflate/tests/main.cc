#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_inflate(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_inflate(c);
    int rc = kv::test_report(c, "inflate_tests");
    kv::mem_shutdown();
    return rc;
}
