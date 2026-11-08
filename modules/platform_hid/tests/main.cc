#include "korvid/core/test.h"

void test_keymap(kv::TestCtx&);
void test_actions(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_keymap(c);
    test_actions(c);
    return kv::test_report(c, "platform_hid_tests");
}
