#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_broadphase(kv::TestCtx&);
void test_pairs(kv::TestCtx&);
void test_resolve(kv::TestCtx&);
void test_compose(kv::TestCtx&);
void test_cast_all(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_broadphase(c);
    test_pairs(c);
    test_resolve(c);
    test_compose(c);
    test_cast_all(c);
    kv::names_shutdown();
    kv::mem_shutdown();
    return kv::test_report(c, "collision_tests");
}
