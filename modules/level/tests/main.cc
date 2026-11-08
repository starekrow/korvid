#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_level(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_level(c);
    kv::names_shutdown();
    kv::mem_shutdown();
    return kv::test_report(c, "level_tests");
}
