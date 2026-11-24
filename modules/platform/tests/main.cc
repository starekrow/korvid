#include "korvid/core/test.h"

void test_io(kv::TestCtx&);
void test_io_write(kv::TestCtx&);
void test_io_budget(kv::TestCtx&);
void test_io_mounts(kv::TestCtx&);
void test_io_make_dirs(kv::TestCtx&);
void test_io_remove_tree(kv::TestCtx&);
void test_io_now(kv::TestCtx&);
void test_io_tasks(kv::TestCtx&);
void test_io_long_ranged_stat(kv::TestCtx&);
void test_io_make_dirs_deep(kv::TestCtx&);
void test_user_dirs(kv::TestCtx&);
void test_wtf8(kv::TestCtx&);
void test_io_list_dir(kv::TestCtx&);
void test_net(kv::TestCtx&);
void test_fs_watch(kv::TestCtx&);
void test_process(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_io(c);
    test_io_write(c);
    test_io_budget(c);
    test_io_mounts(c);
    test_io_make_dirs(c);
    test_io_remove_tree(c);
    test_io_now(c);
    test_io_tasks(c);
    test_io_long_ranged_stat(c);
    test_io_make_dirs_deep(c);
    test_user_dirs(c);
    test_wtf8(c);
    test_io_list_dir(c);
    test_net(c);
    test_fs_watch(c);
    test_process(c);
    return kv::test_report(c, "platform_tests");
}
