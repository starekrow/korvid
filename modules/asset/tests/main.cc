#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_png(kv::TestCtx&);
void test_sound(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_png(c);
    test_sound(c);
    int rc = kv::test_report(c, "asset_tests");
    kv::mem_shutdown();
    return rc;
}
