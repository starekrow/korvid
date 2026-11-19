#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_text(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_text(c);
    kv::names_shutdown();
    kv::mem_shutdown();
    return kv::test_report(c, "sprite_text_tests");
}
