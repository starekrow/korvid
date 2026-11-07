#include "korvid/core/test.h"

void test_png(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_png(c);
    return kv::test_report(c, "asset_tests");
}
