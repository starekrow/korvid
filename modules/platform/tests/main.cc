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
void test_io_errors(kv::TestCtx&);
void test_io_take(kv::TestCtx&);
void test_io_make_dirs_deep(kv::TestCtx&);
void test_user_dirs(kv::TestCtx&);
void test_wtf8(kv::TestCtx&);
void test_io_list_dir(kv::TestCtx&);
void test_net(kv::TestCtx&);
void test_fs_watch(kv::TestCtx&);
void test_process(kv::TestCtx&);
void test_settings(kv::TestCtx&);
void test_wait(kv::TestCtx&);
int  settings_child(int argc, char** argv);
#include <string.h>

int main(int argc, char** argv) {
    // A child the settings test runs: this binary, setting a value and leaving.
    if (argc > 1 && !strcmp(argv[1], "--settings-child")) return settings_child(argc, argv);
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
    test_io_errors(c);
    test_io_take(c);
    test_io_make_dirs_deep(c);
    test_user_dirs(c);
    test_wtf8(c);
    test_io_list_dir(c);
    test_net(c);
    test_fs_watch(c);
    test_process(c);
    test_settings(c);
    test_wait(c);
    return kv::test_report(c, "platform_tests");
}
