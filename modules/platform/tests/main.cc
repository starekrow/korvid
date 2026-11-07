#include "korvid/core/test.h"

void test_keymap(kv::TestCtx&);
void test_io(kv::TestCtx&);
void test_net(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_keymap(c);
    test_io(c);
    test_net(c);
    return kv::test_report(c, "platform_tests");
}
