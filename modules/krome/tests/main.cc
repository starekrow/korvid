#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_tree(kv::TestCtx&);
void test_layout(kv::TestCtx&);
void test_lua(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_tree(c);
    test_layout(c);
    test_lua(c);
    kv::names_shutdown();
    kv::mem_shutdown();
    return kv::test_report(c, "krome_tests");
}
