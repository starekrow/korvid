#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_broadphase(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_broadphase(c);
    kv::names_shutdown();
    kv::mem_shutdown();
    return kv::test_report(c, "collision_tests");
}
