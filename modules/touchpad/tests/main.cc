#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_touchpad_layout(kv::TestCtx&);
void test_touchpad_stick(kv::TestCtx&);
void test_touchpad_buttons(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_touchpad_layout(c);
    test_touchpad_stick(c);
    test_touchpad_buttons(c);
    kv::names_shutdown();
    kv::mem_shutdown();
    return kv::test_report(c, "touchpad_tests");
}
