#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_asset_render(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_asset_render(c);
    int rc = kv::test_report(c, "asset_render_tests");
    kv::names_shutdown();
    kv::mem_shutdown();
    return rc;
}
