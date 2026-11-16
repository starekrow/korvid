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
    kv::names_shutdown();
    kv::mem_shutdown();
    return kv::test_report(c, "krome_tests");
}
