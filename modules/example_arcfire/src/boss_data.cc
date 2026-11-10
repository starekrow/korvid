// example_arcfire: reading a boss file. A boss is its hull, its entry, its parts and its
// phases:
//
//   { "title": "CARRIER", "size": [192, 352], "sheet": [4, 2],
//     "hull": { "half": [52, 168], "offset": [0, -4] },
//     "entry": { "path": [[0, 560], [0, 200]], "speed": 1.4 },
//     "timeout": 45, "score": 20000,
//     "paths": { "bay_left": [[0, 0], [-80, -160], [-260, -760]] },
//     "parts": [
//       { "name": "turret_1", "cell": [0, 0], "at": [-64, 72], "radius": 13, "health": 50,
//         "score": 1000, "pattern": ["ring", "aimed_burst"], "rest": 40 },
//       { "name": "bay_left", "cell": [2, 0], "open": [3, 0], "at": [-36, 16], "radius": 16,
//         "health": 60, "score": 1500, "every": 300,
//         "launch": { "path": "bay_left", "count": 4, "spacing": 10, "sprite": "red", ... } },
//       { "name": "core", "cell": [0, 1], "open": [1, 1], "at": [0, -104], "radius": 18,
//         "health": 200, "score": 5000, "pattern": ["spiral", "fan_storm"], "armored_until": 1,
//         "core": true } ],
//     "phases": [ { "name": "turrets", "ends": ["turret_1", ...], "sway": [40, 0.012] },
//                 { "name": "core", "ends": ["core"], "sway": [70, 0.02] } ] }
//
// Points are world units on the hull (+y up, the hull's middle at 0), `size` the hull's
// sprite, a part's sprite one cell of the sheet drawn at twice its pixels; `timeout` is in
// seconds; the hull's collider is the body the ship must not touch. A part's `parent` names another part it rides on (a turret on a door: the door
// shot away takes it along). The patterns are found in the library at boot.
#include "arcfire.h"
#include "korvid/core/json.h"
#include "korvid/core/memory.h"
#include <stdio.h>
#include <string.h>

namespace kv::arcfire {

namespace {

constexpr f32 kPartPixels = 24.0f, kPartScale = 2.0f; // a sheet's cell, and how large it is drawn

bool boss_fail(char* why, usize cap, const char* name, const char* text) {
    snprintf(why, cap, "boss %s: %s", name, text);
    return false;
}

point2 xy_of(const JsonDoc& doc, const JsonValue* v, point2 dflt) {
    f32 xy[2] = {dflt.x, dflt.y};
    if (v) json_numbers(doc, v, xy, 2);
    return point2{xy[0], xy[1]};
}

i32 part_named(const BossDef& b, const char* name) {
    for (u32 i = 0; i < b.part_count; ++i)
        if (!strcmp(b.parts[i].name, name)) return (i32)i;
    return -1;
}

Look boss_look(const char* s) { return !strcmp(s, "green") ? Look::Green : !strcmp(s, "fighter") ? Look::Fighter : Look::Red; }

} // namespace

bool boss_parse(const char* text, usize len, const char* name, BossDef& out, StageDef& stage, char* why, usize why_cap) {
    MemScope scope;
    JsonDoc doc;
    if (!json_parse(text, len, mem_scratch(), doc)) {
        snprintf(why, why_cap, "boss %s: %s at byte %u", name, doc.error, doc.error_at);
        return false;
    }
    const JsonValue& root = doc.root();
    if (!root.is_object()) return boss_fail(why, why_cap, name, "a boss is an object");
    out = BossDef{};
    snprintf(out.name, sizeof out.name, "%s", name);
    snprintf(out.title, sizeof out.title, "%s", json_string(json_get(doc, root, "title"), name));
    point2 size = xy_of(doc, json_get(doc, root, "size"), point2{128, 128});
    out.size = vec2{size.x, size.y};
    point2 sheet = xy_of(doc, json_get(doc, root, "sheet"), point2{1, 1});
    if (sheet.x < 1.0f || sheet.y < 1.0f) return boss_fail(why, why_cap, name, "a sheet is [columns, rows]");
    out.cell = vec2{1.0f / sheet.x, 1.0f / sheet.y};
    const JsonValue* hull = json_get(doc, root, "hull");
    point2 half = xy_of(doc, hull ? json_get(doc, *hull, "half") : nullptr, point2{size.x * 0.4f, size.y * 0.45f});
    point2 off = xy_of(doc, hull ? json_get(doc, *hull, "offset") : nullptr, point2{0, 0});
    out.hull_half = vec2{half.x, half.y};
    out.hull_offset = vec2{off.x, off.y};
    out.timeout = (u32)(json_number(json_get(doc, root, "timeout"), 45.0) * 60.0);
    out.score = (i32)json_i64(json_get(doc, root, "score"), 10000);

    // The entry: a path from above the field to the hold, at a speed.
    const JsonValue* entry = json_get(doc, root, "entry");
    const JsonValue* entry_path = entry ? json_get(doc, *entry, "path") : nullptr;
    point2 pts[kPathPoints];
    u32 n = 0;
    for (const JsonValue* xy = entry_path ? json_first(doc, *entry_path) : nullptr; xy && n < kPathPoints; xy = json_next(doc, *xy))
        pts[n++] = xy_of(doc, xy, point2{0, 0});
    if (n < 2) return boss_fail(why, why_cap, name, "an entry is a path of two points or more");
    path_build(out.entry, pts, n);
    out.speed = (f32)json_number(json_get(doc, *entry, "speed"), 1.0);

    // Its paths join the stage's, under the boss's name.
    if (const JsonValue* paths = json_get(doc, root, "paths")) {
        for (const JsonValue* p = json_first(doc, *paths); p; p = json_next(doc, *p)) {
            if (stage.path_count == kMaxPaths) return boss_fail(why, why_cap, name, "too many paths with the stage's");
            u32 k = 0;
            for (const JsonValue* xy = json_first(doc, *p); xy && k < kPathPoints; xy = json_next(doc, *xy)) pts[k++] = xy_of(doc, xy, point2{0, 0});
            if (k < 2) return boss_fail(why, why_cap, name, "a path has two points or more");
            snprintf(stage.path_names[stage.path_count], sizeof stage.path_names[0], "%.11s.%.11s", name, p->key);
            path_build(stage.paths[stage.path_count++], pts, k);
        }
    }

    const JsonValue* parts = json_get(doc, root, "parts");
    if (!parts || !parts->is_array()) return boss_fail(why, why_cap, name, "a boss has parts");
    bool has_core = false;
    for (const JsonValue* v = json_first(doc, *parts); v; v = json_next(doc, *v)) {
        if (out.part_count == kMaxParts) return boss_fail(why, why_cap, name, "too many parts");
        PartDef& p = out.parts[out.part_count];
        p = PartDef{};
        snprintf(p.name, sizeof p.name, "%s", json_string(json_get(doc, *v, "name"), ""));
        if (!p.name[0] || part_named(out, p.name) >= 0) return boss_fail(why, why_cap, name, "a part has a name of its own");
        p.at = xy_of(doc, json_get(doc, *v, "at"), point2{0, 0});
        p.parent = -1;
        p.launch = kNoFormation;
        if (const JsonValue* parent = json_get(doc, *v, "parent")) {
            p.parent = part_named(out, json_string(parent, ""));
            if (p.parent < 0) { snprintf(why, why_cap, "boss %s: part %s rides on \"%s\", which is not a part before it", name, p.name, json_string(parent, "")); return false; }
        }
        point2 cell = xy_of(doc, json_get(doc, *v, "cell"), point2{0, 0});
        point2 open = xy_of(doc, json_get(doc, *v, "open"), cell);
        p.uv = rect2{cell.x * out.cell.x, cell.y * out.cell.y, out.cell.x, out.cell.y};
        p.uv_open = rect2{open.x * out.cell.x, open.y * out.cell.y, out.cell.x, out.cell.y};
        p.size = vec2{kPartPixels * kPartScale, kPartPixels * kPartScale};
        p.radius = (f32)json_number(json_get(doc, *v, "radius"), 14.0);
        p.health = (i32)json_i64(json_get(doc, *v, "health"), 40);
        p.score = (i32)json_i64(json_get(doc, *v, "score"), 1000);
        p.rest = (u32)json_u64(json_get(doc, *v, "rest"), 40);
        p.armored_until = (u32)json_u64(json_get(doc, *v, "armored_until"), 0);
        p.core = json_bool(json_get(doc, *v, "core"), false);
        has_core = has_core || p.core;
        if (const JsonValue* pat = json_get(doc, *v, "pattern")) {
            if (pat->is_string()) snprintf(p.patterns[p.pattern_count++], kPatternName, "%s", pat->str);
            else
                for (const JsonValue* x = json_first(doc, *pat); x && p.pattern_count < kMaxPartPatterns; x = json_next(doc, *x))
                    snprintf(p.patterns[p.pattern_count++], kPatternName, "%s", json_string(x, ""));
        }
        for (u32 k = 0; k < kMaxPartPatterns; ++k) p.entries[k] = KV_BML_NONE;
        if (const JsonValue* l = json_get(doc, *v, "launch")) { // a bay: a formation every so often, from where it is
            if (stage.formation_count == kMaxFormations) return boss_fail(why, why_cap, name, "too many formations with the stage's");
            char path[24];
            snprintf(path, sizeof path, "%.11s.%.11s", name, json_string(json_get(doc, *l, "path"), ""));
            FormationDef f{};
            f.path = kMaxPaths;
            for (u32 i = 0; i < stage.path_count; ++i)
                if (!strcmp(stage.path_names[i], path)) f.path = i;
            if (f.path == kMaxPaths) { snprintf(why, why_cap, "boss %s: part %s launches on no path \"%s\"", name, p.name, path); return false; }
            f.count = (u32)json_u64(json_get(doc, *l, "count"), 4);
            f.spacing = (u32)json_u64(json_get(doc, *l, "spacing"), 10);
            f.look = boss_look(json_string(json_get(doc, *l, "sprite"), "red"));
            f.health = (i32)json_i64(json_get(doc, *l, "health"), 1);
            f.score = (i32)json_i64(json_get(doc, *l, "score"), 50);
            f.fire_at = (f32)json_number(json_get(doc, *l, "fire_at"), 0.3);
            f.speed = (f32)json_number(json_get(doc, *l, "speed"), 2.8);
            snprintf(f.pattern, sizeof f.pattern, "%s", json_string(json_get(doc, *l, "pattern"), "aimed"));
            f.entry = KV_BML_NONE;
            p.launch = stage.formation_count;
            stage.formations[stage.formation_count++] = f;
            p.every = (u32)json_u64(json_get(doc, *v, "every"), 300);
        }
        out.total += p.health;
        ++out.part_count;
    }
    if (!has_core) return boss_fail(why, why_cap, name, "a boss has a core");

    const JsonValue* phases = json_get(doc, root, "phases");
    for (const JsonValue* v = phases ? json_first(doc, *phases) : nullptr; v; v = json_next(doc, *v)) {
        if (out.phase_count == kMaxPhases) return boss_fail(why, why_cap, name, "too many phases");
        PhaseDef& ph = out.phases[out.phase_count++];
        ph = PhaseDef{};
        snprintf(ph.name, sizeof ph.name, "%s", json_string(json_get(doc, *v, "name"), ""));
        point2 sway = xy_of(doc, json_get(doc, *v, "sway"), point2{0, 0});
        ph.sway = sway.x;
        ph.sway_rate = sway.y;
        const JsonValue* ends = json_get(doc, *v, "ends");
        for (const JsonValue* x = ends ? json_first(doc, *ends) : nullptr; x; x = json_next(doc, *x)) {
            i32 k = part_named(out, json_string(x, ""));
            if (k < 0) { snprintf(why, why_cap, "boss %s: phase %s ends with \"%s\", which is not a part", name, ph.name, json_string(x, "")); return false; }
            ph.ends |= 1u << k;
        }
        if (!ph.ends) return boss_fail(why, why_cap, name, "a phase ends with parts");
    }
    if (!out.phase_count) return boss_fail(why, why_cap, name, "a boss has phases");
    for (u32 i = 0; i < out.part_count; ++i) {
        const PartDef& p = out.parts[i];
        if (p.armored_until >= out.phase_count)
            { snprintf(why, why_cap, "boss %s: part %s opens in phase %u of %u", name, p.name, p.armored_until, out.phase_count); return false; }
    }
    return true;
}

} // namespace kv::arcfire
