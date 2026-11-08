#include "korvid/core/test.h"

void test_io(kv::TestCtx&);
void test_io_write(kv::TestCtx&);
void test_io_budget(kv::TestCtx&);
void test_io_mounts(kv::TestCtx&);
void test_net(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_io(c);
    test_io_write(c);
    test_io_budget(c);
    test_io_mounts(c);
    test_net(c);
    return kv::test_report(c, "platform_tests");
}
