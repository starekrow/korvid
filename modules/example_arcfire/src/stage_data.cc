// example_arcfire: reading a stage file. A stage is a starting speed, named paths, and
// marks by distance, each one action:
//
//   { "name": "Shoreline", "speed": 1.5,
//     "paths": { "dive_left": [[-200, 420], [-160, 120], [40, -60], [300, -440]] },
//     "marks": [
//       { "at": 200,  "formation": { "path": "dive_left", "count": 6, "spacing": 12, "sprite": "red",
//                                    "health": 2, "score": 100, "pattern": "aimed", "fire_at": 0.4, "speed": 3 } },
//       { "at": 900,  "ground": { "x": -120, "health": 12, "score": 500, "pattern": "ring" } },
//       { "at": 1500, "speed": { "value": 2.2, "over": 300 } },
//       { "at": 4600, "boss": "carrier" },
//       { "at": 5400, "end": true } ] }
//
// Points are field units, +y up; speeds are units a tick; `fire_at` is a fraction of the
// path, where the formation's members start their pattern. A boss mark names a boss of
// assets/bosses/: it comes, and the marks after it wait until it has died or left.
// Patterns are named here and found in the library once both are loaded (stage.cc).
// Marks are sorted by distance here, so the file may list them in any order.
#include "arcfire.h"
#include "korvid/core/json.h"
#include "korvid/core/memory.h"
#include <stdio.h>
#include <string.h>

namespace kv::arcfire {

namespace {

bool fail(char* why, usize cap, const char* text) {
    snprintf(why, cap, "%s", text);
    return false;
}

Look look_named(const char* s) {
    if (!strcmp(s, "red")) return Look::Red;
    if (!strcmp(s, "green")) return Look::Green;
    if (!strcmp(s, "turret")) return Look::Turret;
    return Look::Fighter;
}

// Stable, so marks at one distance run in the order the file lists them; a stage has a
// hundred marks at most.
void sort_marks(Mark* m, u32 n) {
    for (u32 i = 1; i < n; ++i) {
        Mark x = m[i];
        u32 j = i;
        for (; j > 0 && m[j - 1].at > x.at; --j) m[j] = m[j - 1];
        m[j] = x;
    }
}

} // namespace

bool stage_parse(const char* text, usize len, StageDef& out, char* why, usize why_cap) {
    MemScope scope;
    JsonDoc doc;
    if (!json_parse(text, len, mem_scratch(), doc)) {
        snprintf(why, why_cap, "%s at byte %u", doc.error, doc.error_at);
        return false;
    }
    const JsonValue& root = doc.root();
    if (!root.is_object()) return fail(why, why_cap, "a stage is an object");
    out = StageDef{};
    snprintf(out.name, sizeof out.name, "%s", json_string(json_get(doc, root, "name"), "Stage"));
    out.speed = (f32)json_number(json_get(doc, root, "speed"), 1.0);
    snprintf(out.map, sizeof out.map, "%s", json_string(json_get(doc, root, "map"), ""));
    if (const JsonValue* planes = json_get(doc, root, "planes")) {
        out.planes = 0;
        // By name; the low clouds bring their shadows with them (planes.cc's order).
        struct Named { const char* name; u32 bits; };
        static const Named k_named[] = {{"stars", 1u}, {"nebula", 2u}, {"cloud", 4u}, {"low_clouds", 8u | 16u}, {"high_clouds", 32u}};
        for (const JsonValue* v = json_first(doc, *planes); v; v = json_next(doc, *v)) {
            u32 i = 0;
            while (i < KV_COUNTOF(k_named) && strcmp(json_string(v, ""), k_named[i].name)) ++i;
            if (i == KV_COUNTOF(k_named)) return fail(why, why_cap, "a plane is stars, nebula, cloud, low_clouds or high_clouds");
            out.planes |= k_named[i].bits;
        }
    }

    const JsonValue* paths = json_get(doc, root, "paths");
    for (const JsonValue* p = paths ? json_first(doc, *paths) : nullptr; p; p = json_next(doc, *p)) {
        if (out.path_count == kMaxPaths) return fail(why, why_cap, "too many paths");
        point2 pts[kPathPoints];
        u32 n = 0;
        for (const JsonValue* xy = json_first(doc, *p); xy && n < kPathPoints; xy = json_next(doc, *xy)) {
            f32 v[2] = {};
            if (json_numbers(doc, xy, v, 2) != 2) return fail(why, why_cap, "a path point is [x, y]");
            pts[n++] = point2{v[0], v[1]};
        }
        if (n < 2) return fail(why, why_cap, "a path has two points or more");
        snprintf(out.path_names[out.path_count], sizeof out.path_names[0], "%s", p->key);
        path_build(out.paths[out.path_count++], pts, n);
    }

    const JsonValue* marks = json_get(doc, root, "marks");
    if (!marks || !marks->is_array()) return fail(why, why_cap, "a stage has marks");
    bool ended = false;
    for (const JsonValue* m = json_first(doc, *marks); m; m = json_next(doc, *m)) {
        if (out.mark_count == kMaxMarks) return fail(why, why_cap, "too many marks");
        Mark mk{};
        mk.at = (f32)json_number(json_get(doc, *m, "at"), -1.0);
        if (mk.at < 0.0f) return fail(why, why_cap, "a mark has an \"at\"");
        if (const JsonValue* f = json_get(doc, *m, "formation")) {
            if (out.formation_count == kMaxFormations) return fail(why, why_cap, "too many formations");
            FormationDef d{};
            const char* path = json_string(json_get(doc, *f, "path"), "");
            d.path = kMaxPaths;
            for (u32 i = 0; i < out.path_count; ++i)
                if (!strcmp(out.path_names[i], path)) d.path = i;
            if (d.path == kMaxPaths) { snprintf(why, why_cap, "no path \"%s\"", path); return false; }
            d.count = (u32)json_u64(json_get(doc, *f, "count"), 1);
            d.spacing = (u32)json_u64(json_get(doc, *f, "spacing"), 12);
            d.look = look_named(json_string(json_get(doc, *f, "sprite"), "red"));
            d.health = (i32)json_i64(json_get(doc, *f, "health"), 1);
            d.score = (i32)json_i64(json_get(doc, *f, "score"), 100);
            d.fire_at = (f32)json_number(json_get(doc, *f, "fire_at"), 0.5);
            snprintf(d.pattern, sizeof d.pattern, "%s", json_string(json_get(doc, *f, "pattern"), "aimed"));
            d.entry = KV_BML_NONE;
            d.speed = (f32)json_number(json_get(doc, *f, "speed"), 3.0);
            mk.kind = MarkKind::Formation;
            mk.index = out.formation_count;
            out.formations[out.formation_count++] = d;
        } else if (const JsonValue* gr = json_get(doc, *m, "ground")) {
            if (out.ground_count == kMaxGrounds) return fail(why, why_cap, "too many ground rows");
            GroundDef d{};
            d.x = (f32)json_number(json_get(doc, *gr, "x"), 0.0);
            d.health = (i32)json_i64(json_get(doc, *gr, "health"), 10);
            d.score = (i32)json_i64(json_get(doc, *gr, "score"), 500);
            snprintf(d.pattern, sizeof d.pattern, "%s", json_string(json_get(doc, *gr, "pattern"), "ring"));
            d.entry = KV_BML_NONE;
            mk.kind = MarkKind::Ground;
            mk.index = out.ground_count;
            out.grounds[out.ground_count++] = d;
        } else if (const JsonValue* sp = json_get(doc, *m, "speed")) {
            mk.kind = MarkKind::Speed;
            mk.value = (f32)json_number(json_get(doc, *sp, "value"), out.speed);
            mk.over = (f32)json_number(json_get(doc, *sp, "over"), 0.0);
        } else if (const JsonValue* boss = json_get(doc, *m, "boss")) {
            // The boss's name; boot finds its definition.
            if (out.boss_mark_count == kMaxBosses) return fail(why, why_cap, "too many boss marks");
            mk.kind = MarkKind::Boss;
            mk.index = out.boss_mark_count;
            snprintf(out.boss_names[out.boss_mark_count++], sizeof out.boss_names[0], "%s", json_string(boss, ""));
        } else if (json_bool(json_get(doc, *m, "end"), false)) {
            mk.kind = MarkKind::End;
            if (mk.at <= 0.0f) return fail(why, why_cap, "the end mark is past the start"); // a stage that loops must have a length
            out.length = mk.at;
            ended = true;
        } else return fail(why, why_cap, "a mark is a formation, a ground row, a speed, a boss or the end");
        out.marks[out.mark_count++] = mk;
    }
    if (!ended) return fail(why, why_cap, "a stage has an end mark");
    sort_marks(out.marks, out.mark_count);
    return true;
}

} // namespace kv::arcfire
