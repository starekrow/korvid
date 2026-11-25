#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_tree(kv::TestCtx&);
void test_layout(kv::TestCtx&);
void test_lua(kv::TestCtx&);
void test_view(kv::TestCtx&);
void test_shapes(kv::TestCtx&);
void test_kimb(kv::TestCtx&);
void test_defs(kv::TestCtx&);
void test_edit(kv::TestCtx&);
void test_bound(kv::TestCtx&);
void test_behave(kv::TestCtx&);
void test_virtual(kv::TestCtx&);
void test_sparse(kv::TestCtx&);
void test_sparse_text(kv::TestCtx&);
void test_set(kv::TestCtx&);
void test_hooks(kv::TestCtx&);
void test_rebuild(kv::TestCtx&);
void test_reload(kv::TestCtx&);
void test_grips(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_tree(c);
    test_layout(c);
    test_lua(c);
    test_view(c);
    test_shapes(c);
    test_kimb(c);
    test_defs(c);
    test_edit(c);
    test_bound(c);
    test_behave(c);
    test_virtual(c);
    test_sparse(c);
    test_sparse_text(c);
    test_set(c);
    test_hooks(c);
    test_rebuild(c);
    test_reload(c);
    test_grips(c);
    kv::names_shutdown();
    kv::mem_shutdown();
    return kv::test_report(c, "krome_tests");
}
