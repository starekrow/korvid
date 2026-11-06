#include "korvid/core/test.h"

void test_keymap(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_keymap(c);
    return kv::test_report(c, "platform_tests");
}
