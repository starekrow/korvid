#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_cli_fs(kv::TestCtx&);
void test_cli_ascii(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_cli_fs(c);
    test_cli_ascii(c);
    kv::names_shutdown();
    kv::mem_shutdown();
    return kv::test_report(c, "cli_tests");
}
