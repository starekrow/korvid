#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_png(kv::TestCtx&);
void test_png_encode(kv::TestCtx&);
void test_ppm(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_png(c);
    test_png_encode(c);
    test_ppm(c);
    int rc = kv::test_report(c, "image_tests");
    kv::mem_shutdown();
    return rc;
}
