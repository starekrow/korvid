#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_mixer(kv::TestCtx&);
void test_audio_module(kv::TestCtx&);
void test_asset_scopes(kv::TestCtx&);
void test_fades(kv::TestCtx&);
void test_music(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_mixer(c);
    test_audio_module(c);
    test_asset_scopes(c);
    test_fades(c);
    test_music(c);
    kv::names_shutdown();
    kv::mem_shutdown();
    return kv::test_report(c, "audio_tests");
}
