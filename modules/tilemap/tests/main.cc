#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_tilemap_parse(kv::TestCtx&);
void test_tilemap_load(kv::TestCtx&);
void test_tilemap_move(kv::TestCtx&);
void test_tilemap_path(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_tilemap_parse(c);
    test_tilemap_load(c);
    test_tilemap_move(c);
    test_tilemap_path(c);
    kv::names_shutdown();
    kv::mem_shutdown();
    return kv::test_report(c, "tilemap_tests");
}
