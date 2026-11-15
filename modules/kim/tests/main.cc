#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_kim(kv::TestCtx&);
void test_kim_errors(kv::TestCtx&);
void test_kim_check(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_kim(c);
    test_kim_errors(c);
    test_kim_check(c);
    int rc = kv::test_report(c, "kim_tests");
    kv::mem_shutdown();
    return rc;
}
