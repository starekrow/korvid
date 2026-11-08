#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_handle(kv::TestCtx&);
void test_event(kv::TestCtx&);
void test_tick(kv::TestCtx&);
void test_module(kv::TestCtx&);
void test_memory(kv::TestCtx&);
void test_map(kv::TestCtx&);
void test_name(kv::TestCtx&);
void test_random(kv::TestCtx&);
void test_component(kv::TestCtx&);
void test_kind(kv::TestCtx&);
void test_context(kv::TestCtx&);
void test_schedule(kv::TestCtx&);
void test_handler(kv::TestCtx&);
void test_trigger(kv::TestCtx&);
void test_job(kv::TestCtx&);
void test_parallel(kv::TestCtx&);
void test_heap(kv::TestCtx&);
void test_profile(kv::TestCtx&);
void test_json(kv::TestCtx&);
void test_idle(kv::TestCtx&);
void test_pause(kv::TestCtx&);
void test_qualify(kv::TestCtx&);
void test_utf8(kv::TestCtx&);
void test_hash(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_memory(c);
    test_handle(c);
    test_event(c);
    test_tick(c);
    test_module(c);
    test_map(c);
    test_name(c);
    test_random(c);
    test_component(c);
    test_kind(c);
    test_hash(c);
    test_context(c);
    test_schedule(c);
    test_handler(c);
    test_trigger(c);
    test_job(c);
    test_parallel(c);
    test_heap(c);
    test_profile(c);
    test_json(c);
    test_utf8(c);
    test_idle(c);
    test_pause(c);
    test_qualify(c);
    kv::names_shutdown();
    kv::mem_shutdown(); // the leak report, if anything is left
    return kv::test_report(c, "core_tests");
}
