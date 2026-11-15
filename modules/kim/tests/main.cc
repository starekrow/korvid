#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_kim(kv::TestCtx&);
void test_kim_errors(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_kim(c);
    test_kim_errors(c);
    int rc = kv::test_report(c, "kim_tests");
    kv::mem_shutdown();
    return rc;
}
