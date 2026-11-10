// The stage file: the shipped one parses whole; marks are sorted by distance, keeping the
// file's order at equal distances; names resolve; what is not a stage says why.
#include "../src/stage_data.cc"
#include "korvid/core/test.h"
#include "korvid/platform/io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

// A staged asset of this module, beside the test executable.
char* slurp(const char* name, kv::usize* len) {
    char exe[512];
    if (!kv::platform_exe_dir(exe, sizeof exe)) return nullptr;
    char path[700];
    snprintf(path, sizeof path, "%sassets/example_arcfire/%s", exe, name);
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* text = (char*)malloc((size_t)n + 1);
    *len = fread(text, 1, (size_t)n, f);
    fclose(f);
    text[*len] = 0;
    return text;
}

bool parses(const char* text, kv::arcfire::StageDef& def, char* why, kv::usize cap) {
    return kv::arcfire::stage_parse(text, strlen(text), def, why, cap);
}

} // namespace

void test_stage(kv::TestCtx& c) {
    using namespace kv;
    using namespace kv::arcfire;
    c.begin("stage");
    static StageDef def; // large: not on the stack
    char why[128] = "";

    usize len = 0;
    char* text = slurp("stages/shoreline.stage.json", &len);
    KV_CHECK(c, text != nullptr);
    if (text) {
        KV_CHECK(c, stage_parse(text, len, def, why, sizeof why));
        KV_CHECK(c, def.formation_count == 8 && def.ground_count == 3 && def.length > 0.0f);
        for (u32 i = 0; i < def.formation_count; ++i) KV_CHECK(c, def.formations[i].pattern[0] != 0 && def.formations[i].entry == KV_BML_NONE);
        u32 speeds = 0;
        for (u32 i = 0; i < def.mark_count; ++i) {
            speeds += def.marks[i].kind == MarkKind::Speed;
            if (i) KV_CHECK(c, def.marks[i - 1].at <= def.marks[i].at);
        }
        KV_CHECK(c, speeds == 2 && def.marks[def.mark_count - 1].kind == MarkKind::End);
        for (u32 i = 0; i < def.formation_count; ++i) KV_CHECK(c, def.formations[i].path < def.path_count && def.paths[def.formations[i].path].length > 0.0f);
        free(text);
    }

    // Out of order in the file; equal distances keep the file's order.
    KV_CHECK(c, parses(R"({"speed": 2, "paths": {"p": [[0, 0], [0, 10]]}, "marks": [
        {"at": 50, "end": true},
        {"at": 10, "speed": {"value": 3}},
        {"at": 10, "formation": {"path": "p", "count": 4, "sprite": "green", "pattern": "spiral"}},
        {"at": 5, "ground": {"x": 12}}]})", def, why, sizeof why));
    KV_CHECK(c, def.speed == 2.0f && def.length == 50.0f && def.mark_count == 4);
    KV_CHECK(c, def.marks[0].kind == MarkKind::Ground && def.marks[1].kind == MarkKind::Speed && def.marks[2].kind == MarkKind::Formation &&
                def.marks[3].kind == MarkKind::End);
    KV_CHECK(c, def.formations[0].count == 4 && def.formations[0].look == Look::Green && def.grounds[0].x == 12.0f);
    KV_CHECK(c, !strcmp(def.formations[0].pattern, "spiral") && !strcmp(def.grounds[0].pattern, "ring")); // ring: a ground row's default

    // What is not a stage.
    KV_CHECK(c, !parses(R"({"marks": [{"at": 1, "formation": {"path": "nowhere"}}, {"at": 2, "end": true}]})", def, why, sizeof why) &&
                strstr(why, "nowhere"));
    KV_CHECK(c, !parses(R"({"marks": [{"at": 1, "speed": {"value": 1}}]})", def, why, sizeof why) && strstr(why, "end"));
    KV_CHECK(c, !parses(R"({"paths": {"p": [[0, 0]]}, "marks": []})", def, why, sizeof why) && strstr(why, "two points"));
    KV_CHECK(c, !parses(R"({"marks": [{"at": 1, "dance": true}]})", def, why, sizeof why));
    KV_CHECK(c, !parses(R"({"marks": [{"at": 0, "end": true}]})", def, why, sizeof why) && strstr(why, "past the start"));
    KV_CHECK(c, !parses("[1, 2", def, why, sizeof why));
}
