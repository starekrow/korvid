#include "korvid/core/test.h"

void test_sprite_world(kv::TestCtx&);
void test_extract(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_sprite_world(c);
    test_extract(c);
    return kv::test_report(c, "sprite_tests");
}
