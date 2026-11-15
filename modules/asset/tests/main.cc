#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_sound(kv::TestCtx&);
void test_font(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_sound(c);
    test_font(c);
    int rc = kv::test_report(c, "asset_tests");
    kv::names_shutdown();
    kv::mem_shutdown();
    return rc;
}
