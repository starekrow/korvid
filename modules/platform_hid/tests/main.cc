#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_keymap(kv::TestCtx&);
void test_actions(kv::TestCtx&);
void test_text_events(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_keymap(c);
    test_actions(c);
    test_text_events(c);
    int rc = kv::test_report(c, "platform_hid_tests");
    kv::names_shutdown();
    kv::mem_shutdown();
    return rc;
}
