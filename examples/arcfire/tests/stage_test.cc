// The stage file: the shipped one parses whole; marks are sorted by distance, keeping the
// file's order at equal distances; names resolve; what is not a stage says why. The boss
// files: the carrier parses whole, its paths and bays' formations join the stage's, and
// what is not a boss says why.
#include "../src/stage_data.cc"
#include "../src/boss_data.cc"
#include "korvid/core/test.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/platform/io.h"
#include <stdlib.h>
#include <string.h>

namespace {

// A cooked asset of this module, from the test's own output tree.
char* slurp(const char* name, kv::usize* len) {
    char assets[1024];
    if (!kv::platform_assets_dir(assets, sizeof assets)) return nullptr;
    char path[1200];
    kv::format(path, sizeof path, "%s/arcfire/%s", assets, name);
    kv::IoBytes file;
    if (!kv::io_read_now(path, &file)) return nullptr;
    char* text = (char*)malloc(file.size + 1); // the caller's to free
    memcpy(text, file.data, file.size + 1);    // the NUL after the bytes too
    *len = file.size;
    file.free();
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
        KV_CHECK(c, def.formation_count == 37 && def.ground_count == 0 && def.length > 0.0f && !strcmp(def.map, "arcfire/maps/shoreline.tmj") && def.planes == (8u | 16u | 32u)); // the low clouds with their shadows, the high
        for (u32 i = 0; i < def.formation_count; ++i) KV_CHECK(c, def.formations[i].pattern[0] != 0 && def.formations[i].entry == KV_BML_NONE);
        u32 speeds = 0;
        for (u32 i = 0; i < def.mark_count; ++i) {
            speeds += def.marks[i].kind == MarkKind::Speed;
            if (i) KV_CHECK(c, def.marks[i - 1].at <= def.marks[i].at);
        }
        KV_CHECK(c, speeds == 8 && def.marks[def.mark_count - 1].kind == MarkKind::End && def.length == 18400.0f);
        KV_CHECK(c, def.boss_mark_count == 2 && !strcmp(def.boss_names[0], "carrier") && !strcmp(def.boss_names[1], "battleship"));
        for (u32 i = 0; i < def.formation_count; ++i) KV_CHECK(c, def.formations[i].path < def.path_count && def.paths[def.formations[i].path].length > 0.0f);
        free(text);
    }

    // Orbit: no map, the gas under the stars, segments, two bosses on one mark, debris that
    // never fires.
    text = slurp("stages/orbit.stage.json", &len);
    KV_CHECK(c, text != nullptr);
    if (text) {
        KV_CHECK(c, stage_parse(text, len, def, why, sizeof why));
        KV_CHECK(c, !def.map[0] && (def.planes & kPlaneShader) && def.segment_count == 12 && def.length == 18400.0f);
        KV_CHECK(c, def.boss_mark_count == 3 && !strcmp(def.boss_names[0], "gunship_left") && !strcmp(def.boss_names[1], "gunship_right") &&
                    !strcmp(def.boss_names[2], "station"));
        u32 bosses = 0, debris = 0;
        for (u32 i = 0; i < def.mark_count; ++i)
            if (def.marks[i].kind == MarkKind::Boss) { KV_CHECK(c, def.marks[i].value == (bosses ? 1.0f : 2.0f)); ++bosses; }
        for (u32 i = 0; i < def.formation_count; ++i)
            if (def.formations[i].look == Look::Debris) { ++debris; KV_CHECK(c, def.formations[i].pattern[0] == 0); }
        KV_CHECK(c, bosses == 2 && debris > 0);
        for (u32 i = 0; i < def.segment_count; ++i) KV_CHECK(c, def.segments[i].cell <= 1 && def.segments[i].pattern[0] != 0);
        free(text);
    }
    KV_CHECK(c, !parses(R"({"marks": [{"at": 1, "segment": {"cell": 2}}, {"at": 2, "end": true}]})", def, why, sizeof why) && strstr(why, "cell"));
    KV_CHECK(c, !parses(R"({"marks": [{"at": 1, "boss": []}, {"at": 2, "end": true}]})", def, why, sizeof why) && strstr(why, "names a boss"));

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
    KV_CHECK(c, def.formations[0].drop == DropNone);

    // Item carriers: a formation's drop by name; one the game does not know says so.
    KV_CHECK(c, parses(R"({"paths": {"p": [[0, 0], [0, 10]]}, "marks": [{"at": 50, "end": true},
        {"at": 10, "formation": {"path": "p", "drop": "lance"}}, {"at": 20, "formation": {"path": "p", "drop": "egg"}}]})", def, why, sizeof why));
    KV_CHECK(c, def.formations[0].drop == (u32)DropWeapon + (u32)WeaponLance && def.formations[1].drop == DropEgg);
    KV_CHECK(c, !parses(R"({"paths": {"p": [[0, 0], [0, 10]]}, "marks": [{"at": 50, "end": true},
        {"at": 10, "formation": {"path": "p", "drop": "cake"}}]})", def, why, sizeof why) && strstr(why, "no drop \"cake\""));

    // What is not a stage.
    KV_CHECK(c, !parses(R"({"marks": [{"at": 1, "formation": {"path": "nowhere"}}, {"at": 2, "end": true}]})", def, why, sizeof why) &&
                strstr(why, "nowhere"));
    KV_CHECK(c, !parses(R"({"marks": [{"at": 1, "speed": {"value": 1}}]})", def, why, sizeof why) && strstr(why, "end"));
    KV_CHECK(c, !parses(R"({"paths": {"p": [[0, 0]]}, "marks": []})", def, why, sizeof why) && strstr(why, "two points"));
    KV_CHECK(c, !parses(R"({"marks": [{"at": 1, "dance": true}]})", def, why, sizeof why));
    KV_CHECK(c, !parses(R"({"marks": [{"at": 0, "end": true}]})", def, why, sizeof why) && strstr(why, "past the start"));
    KV_CHECK(c, !parses("[1, 2", def, why, sizeof why));
}

void test_boss(kv::TestCtx& c) {
    using namespace kv;
    using namespace kv::arcfire;
    c.begin("boss");
    static StageDef stage; // large: not on the stack
    static BossDef b;
    char why[160] = "";

    usize len = 0;
    char* text = slurp("bosses/carrier.boss.json", &len);
    KV_CHECK(c, text != nullptr);
    if (text) {
        stage = StageDef{};
        KV_CHECK(c, boss_parse(text, len, "carrier", b, stage, why, sizeof why));
        KV_CHECK(c, !strcmp(b.title, "CARRIER") && b.part_count == 7 && b.phase_count == 2 && b.timeout == 60 * 60);
        // Its two bays' paths and formations are the stage's now, under its name.
        KV_CHECK(c, stage.path_count == 2 && !strcmp(stage.path_names[0], "carrier.bay_left") && stage.formation_count == 2);
        u32 cores = 0, launchers = 0;
        for (u32 i = 0; i < b.part_count; ++i) {
            cores += b.parts[i].core;
            launchers += b.parts[i].launch != kNoFormation;
        }
        KV_CHECK(c, cores == 1 && launchers == 2);
        // The first phase ends with the four turrets; the core opens in the second.
        KV_CHECK(c, b.phases[0].ends == 0xFu && b.phases[1].ends == 0x40u && b.parts[6].core && b.parts[6].armored_until == 1);
        KV_CHECK(c, b.total == 4 * 102 + 2 * 150 + 390 && b.parts[6].pattern_count == 2 && !strcmp(b.parts[6].patterns[1], "fan_storm"));
        KV_CHECK(c, !strcmp(b.stomp, "carrier_stomp") && b.stomp_ticks == 300 && b.dive == 260.0f);
        KV_CHECK(c, b.parts[4].drop == DropPower && b.parts[5].drop == DropPower && b.parts[6].drop == DropNone); // the bays carry power
        free(text);
    }
    // The battleship: three phases, the bridge its core, opening in the last.
    text = slurp("bosses/battleship.boss.json", &len);
    KV_CHECK(c, text != nullptr);
    if (text) {
        stage = StageDef{};
        KV_CHECK(c, boss_parse(text, len, "battleship", b, stage, why, sizeof why));
        KV_CHECK(c, b.part_count == 10 && b.phase_count == 3 && b.timeout == 90 * 60 && stage.formation_count == 0);
        KV_CHECK(c, b.phases[0].ends == 0x3u && b.phases[1].ends == 0x180u && b.parts[9].core && b.parts[9].armored_until == 2);
        KV_CHECK(c, !strcmp(b.stomp, "wall_walk"));
        free(text);
    }

    // Orbit's: the gunships (two phases, the gun their core) and the station core (three,
    // six emitters, four weapons, the core opening last).
    const char* const orbit[] = {"gunship_left", "gunship_right", "station"};
    for (const char* name : orbit) {
        char path[64];
        format(path, sizeof path, "bosses/%s.boss.json", name);
        text = slurp(path, &len);
        KV_CHECK(c, text != nullptr);
        if (!text) continue;
        stage = StageDef{};
        KV_CHECK(c, boss_parse(text, len, name, b, stage, why, sizeof why));
        bool station = !strcmp(name, "station");
        KV_CHECK(c, b.part_count == (station ? 11u : 3u) && b.phase_count == (station ? 3u : 2u) && b.parts[b.part_count - 1].core);
        KV_CHECK(c, !strcmp(b.stomp, "curtain"));
        free(text);
    }

    // What is not a boss says why.
    auto fails_with = [&](const char* json, const char* says) {
        stage = StageDef{};
        why[0] = 0;
        bool ok = boss_parse(json, strlen(json), "x", b, stage, why, sizeof why);
        if (!ok && !strstr(why, says)) KV_INFO("test", "boss says: %s", why);
        return !ok && strstr(why, says) != nullptr;
    };
    const char* entry = R"("entry": {"path": [[0, 400], [0, 200]]})";
    char doc[512];
    format(doc, sizeof doc, R"({%s, "parts": [{"name": "a", "radius": 8}], "phases": [{"ends": ["a"]}]})", entry);
    KV_CHECK(c, fails_with(doc, "a boss has a core"));
    format(doc, sizeof doc, R"({%s, "parts": [{"name": "a", "core": true, "parent": "b"}], "phases": [{"ends": ["a"]}]})", entry);
    KV_CHECK(c, fails_with(doc, "rides on \"b\""));
    format(doc, sizeof doc, R"({%s, "parts": [{"name": "a", "core": true}], "phases": [{"name": "p", "ends": ["nope"]}]})", entry);
    KV_CHECK(c, fails_with(doc, "ends with \"nope\""));
    format(doc, sizeof doc, R"({%s, "parts": [{"name": "a", "core": true, "armored_until": 2}], "phases": [{"ends": ["a"]}]})", entry);
    KV_CHECK(c, fails_with(doc, "opens in phase 2 of 1"));
    format(doc, sizeof doc, R"({%s, "parts": [{"name": "a", "core": true, "launch": {"path": "none"}}], "phases": [{"ends": ["a"]}]})", entry);
    KV_CHECK(c, fails_with(doc, "launches on no path"));
    KV_CHECK(c, fails_with(R"({"parts": []})", "an entry is a path"));
    format(doc, sizeof doc, R"({%s, "parts": [{"name": "a", "core": true}, {"name": "a"}], "phases": [{"ends": ["a"]}]})", entry);
    KV_CHECK(c, fails_with(doc, "a name of its own"));
    format(doc, sizeof doc, R"({%s, "parts": [{"name": "a", "core": true}], "phases": [{"ends": ["a"]}]})", entry);
    KV_CHECK(c, fails_with(doc, "a boss has a stomp"));
}
