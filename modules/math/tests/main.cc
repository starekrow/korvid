#include "korvid/core/test.h"

void test_vec2(kv::TestCtx&);
void test_affine2(kv::TestCtx&);
void test_scalar(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_vec2(c);
    test_affine2(c);
    test_scalar(c);
    return kv::test_report(c, "math_tests");
}
