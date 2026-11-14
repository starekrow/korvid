#include "korvid/core/test.h"

void test_world_apply(kv::TestCtx&);
void test_world_materials(kv::TestCtx&);
void test_world_runs(kv::TestCtx&);
void test_world_push(kv::TestCtx&);
void test_world_sort_check(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_world_apply(c);
    test_world_materials(c);
    test_world_runs(c);
    test_world_push(c);
    test_world_sort_check(c);
    return kv::test_report(c, "render_tests");
}
