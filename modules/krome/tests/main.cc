#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_tree(kv::TestCtx&);
void test_layout(kv::TestCtx&);
void test_lua(kv::TestCtx&);
void test_view(kv::TestCtx&);
void test_kimb(kv::TestCtx&);
void test_defs(kv::TestCtx&);
void test_edit(kv::TestCtx&);
void test_bound(kv::TestCtx&);
void test_behave(kv::TestCtx&);
void test_virtual(kv::TestCtx&);
void test_hooks(kv::TestCtx&);
void test_rebuild(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_tree(c);
    test_layout(c);
    test_lua(c);
    test_view(c);
    test_kimb(c);
    test_defs(c);
    test_edit(c);
    test_bound(c);
    test_behave(c);
    test_virtual(c);
    test_hooks(c);
    test_rebuild(c);
    kv::names_shutdown();
    kv::mem_shutdown();
    return kv::test_report(c, "krome_tests");
}
