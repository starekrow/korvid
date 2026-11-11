// The fence posts: a step flipbook shows frame i for [i/fps, (i+1)/fps) and never frame 0
// at the end of a loop; Once holds the last key and finishes once; events fire exactly
// once per crossing forward, backward and across a wrap, and a key at 0 fires at the
// start of every loop; PingPong reverses and counts returns; the Sim clock holds on a
// paused tick and Always does not; a stale or pending clip leaves a player alone; the
// file format parses grids, keys and events and rejects what it should; a position
// track drives a child's Local2 and the scene module composes it after.
#include "korvid/anim/module.h"
#include "korvid/anim/sheet.h"
#include "korvid/asset/scope.h"
#include "korvid/save/writer.h"
#include "korvid/asset/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/scene/module.h"
#include "korvid/sprite/module.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"
#include "korvid/sprite/world.h"
#include <math.h>
#include <string.h>

namespace {

using namespace kv;

constinit const KindDesc k_kinds[] = {
    {.name = ".thing", .components = "Transform2, Sprite, AnimPlayer", .capacity = 64},
    {.name = ".child", .components = "Transform2, Sprite, Local2, Parent, AnimPlayer", .capacity = 64},
};

struct Seen { u32 events, finished; u64 last_name; Handle last_entity; };
constinit Seen g = {};
void on_event(TickContext&, Span<const AnimEvent> evs) { for (const AnimEvent& e : evs) { ++g.events; g.last_name = e.name; g.last_entity = e.entity; } }
void on_finished(TickContext&, Span<const AnimFinished> fs) { g.finished += fs.len; }
constinit const HandlerDesc k_handlers[] = {
    {.name = ".on_event", KV_ON(AnimEvent, on_event)},
    {.name = ".on_finished", KV_ON(AnimFinished, on_finished)},
};
KV_MODULE_DEFINE(k_a) = {.name = "a", .kinds = k_kinds, .handlers = k_handlers};

bool near(f32 a, f32 b, f32 eps = 1e-4f) { return fabsf(a - b) < eps; }

// A four-frame flipbook over one second with an event at the start and one at 0.5.
Handle make_flipbook(Engine& e, AnimLoop loop) {
    static AnimKey uv[4];
    for (u32 i = 0; i < 4; ++i) { uv[i].time = 0.25f * (f32)i; uv[i].v[0] = 0.25f * (f32)i; uv[i].v[1] = 0; uv[i].v[2] = 0.25f; uv[i].v[3] = 1; }
    static AnimKey ev[2];
    ev[0].time = 0.0f; ev[0].event = name_intern("start");
    ev[1].time = 0.5f; ev[1].event = name_intern("half");
    const AnimTrackDesc tracks[] = {
        {.channel = AnimChannel::Uv, .interp = AnimInterp::Step, .keys = uv},
        {.channel = AnimChannel::Event, .keys = ev},
    };
    AnimClipDesc d;
    d.name = "flipbook"; d.duration = 1.0f; d.loop = loop; d.tracks = tracks;
    return anim_create(e, d);
}

} // namespace

void test_anim(kv::TestCtx& c) {
    using namespace kv;
    c.begin("anim");

    const ModuleDesc* const list[] = {&sprite_module, &scene_module, &asset_module, &anim_module, &k_a};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    Table* things = engine_table(e, "a.thing");
    Table* kids   = engine_table(e, "a.child");
    u64 tick = 0;
    auto run = [&](f64 dt = 0.1, bool paused = false) { e.phases.run_tick(e, e.events, dt, paused ? tick : tick++, paused); };

    // ---- the descriptor path and sampling -------------------------------------------
    Handle loop = make_flipbook(e, AnimLoop::Loop);
    const AnimClip* lc = anim_clip(e, loop);
    KV_CHECK(c, lc && lc->state == AssetState::Ready && near(lc->duration, 1.0f) && lc->tracks.len == 2 && lc->keys.len == 6);
    f32 v[4];
    KV_CHECK(c, anim_sample(*lc, lc->tracks[0], 0.0f, v) && near(v[0], 0.0f));
    KV_CHECK(c, anim_sample(*lc, lc->tracks[0], 0.2499f, v) && near(v[0], 0.0f));  // frame 0 up to the post
    KV_CHECK(c, anim_sample(*lc, lc->tracks[0], 0.25f, v) && near(v[0], 0.25f));   // frame 1 from it
    KV_CHECK(c, anim_sample(*lc, lc->tracks[0], 0.999f, v) && near(v[0], 0.75f));  // the last holds
    KV_CHECK(c, !anim_sample(*lc, lc->tracks[1], 0.5f, v));                        // events do not sample
    AnimClipDesc bad;
    static const AnimKey out_of_order[] = {{.time = 0.5f}, {.time = 0.25f}};
    const AnimTrackDesc bad_tracks[] = {{.channel = AnimChannel::Tint, .keys = out_of_order}};
    bad.name = "bad"; bad.tracks = bad_tracks;
    KV_CHECK(c, anim_create(e, bad).is_null());

    // ---- a looping player: frames at the posts, the wrap carries the remainder ------
    g = Seen{};
    Handle t1 = things->spawn(transform2_at(point2{0, 0}), Sprite{}, anim_player(loop));
    run(0.1); // t = 0.1: frame 0; the start event fired (closed start)
    KV_CHECK(c, near(things->get<AnimPlayer>(t1)->time, 0.1f) && near(things->get<Sprite>(t1)->uv.x, 0.0f));
    run(0.1); KV_CHECK(c, g.events == 1 && g.last_name == name_intern("start").hash && g.last_entity == t1); // delivered a tick later
    run(0.1); // 0.3: frame 1
    KV_CHECK(c, near(things->get<Sprite>(t1)->uv.x, 0.25f));
    run(0.25); // 0.55: crossed 0.5: "half"
    run(0.1);  // 0.65 (delivers)
    KV_CHECK(c, g.events == 2 && g.last_name == name_intern("half").hash);
    run(0.3);  // 0.95: frame 3
    KV_CHECK(c, near(things->get<Sprite>(t1)->uv.x, 0.75f));
    run(0.1);  // 1.05 -> wraps to 0.05: frame 0 again, loops 1, "start" fires for the new loop
    const AnimPlayer* p1 = things->get<AnimPlayer>(t1);
    KV_CHECK(c, near(p1->time, 0.05f) && p1->loops == 1 && near(things->get<Sprite>(t1)->uv.x, 0.0f));
    run(0.1);
    KV_CHECK(c, g.events == 3 && g.last_name == name_intern("start").hash);
    // Landing exactly on the end never shows frame 0 at t == 1: it wraps to 0 and shows
    // frame 0 as the start of the next loop, with time 0, not 1.
    things->get<AnimPlayer>(t1)->time = 0.75f;
    run(0.25);
    KV_CHECK(c, near(things->get<AnimPlayer>(t1)->time, 0.0f) && things->get<AnimPlayer>(t1)->loops == 2);
    things->destroy(t1);

    // ---- Once: holds the last frame, finishes once ---------------------------------
    g = Seen{};
    Handle once = make_flipbook(e, AnimLoop::Once);
    Handle t2 = things->spawn(transform2_at(point2{0, 0}), Sprite{}, anim_player(once));
    run(0.6); run(0.6); // 1.2 -> clamped at 1.0, Done
    const AnimPlayer* p2 = things->get<AnimPlayer>(t2);
    KV_CHECK(c, p2->state == AnimState::Done && near(p2->time, 1.0f) && near(things->get<Sprite>(t2)->uv.x, 0.75f));
    run(0.6); run(0.6);
    KV_CHECK(c, g.finished == 1 && g.events == 2); // start and half, each once; finished once
    things->destroy(t2);

    // ---- reverse: events fire going backward too, and a reverse Once stops at 0 ----
    g = Seen{};
    Handle t3 = things->spawn(transform2_at(point2{0, 0}), Sprite{}, anim_player(once, -1.0f, AnimClock::Sim, 1.0f));
    run(0.4); // 0.6: frame 2, nothing crossed (0.5 is ahead)
    KV_CHECK(c, near(things->get<Sprite>(t3)->uv.x, 0.5f));
    run(0.2); // 0.4: crossed 0.5 backward
    run(0.5); // 0 -> Done; "start" at 0 is included going backward (closed at the end)
    run(0.1);
    KV_CHECK(c, g.events == 2 && g.finished == 1 && things->get<AnimPlayer>(t3)->state == AnimState::Done);
    things->destroy(t3);

    // ---- PingPong: reverses at the ends, counts a loop per return to the start -----
    g = Seen{};
    Handle pp = make_flipbook(e, AnimLoop::PingPong);
    Handle t4 = things->spawn(transform2_at(point2{0, 0}), Sprite{}, anim_player(pp));
    run(0.9); run(0.2); // 1.1 -> 0.9 going back
    const AnimPlayer* p4 = things->get<AnimPlayer>(t4);
    KV_CHECK(c, near(p4->time, 0.9f) && p4->rate < 0.0f && p4->loops == 0);
    run(0.95); // -0.05 -> 0.05 going forward, one return
    KV_CHECK(c, near(p4->time, 0.05f) && p4->rate > 0.0f && p4->loops == 1);
    run(0.1);
    KV_CHECK(c, g.events == 4); // start, half (forward), half (back), start (at 0, back, closed at the end)
    things->destroy(t4);

    // ---- clocks and states ---------------------------------------------------------
    Handle t5 = things->spawn(transform2_at(point2{0, 0}), Sprite{}, anim_player(loop, 1.0f, AnimClock::Sim));
    Handle t6 = things->spawn(transform2_at(point2{0, 0}), Sprite{}, anim_player(loop, 1.0f, AnimClock::Always));
    run(0.1, true); // a paused tick
    KV_CHECK(c, near(things->get<AnimPlayer>(t5)->time, 0.0f) && near(things->get<AnimPlayer>(t6)->time, 0.1f));
    things->get<AnimPlayer>(t5)->state = AnimState::Paused;
    run(0.1);
    KV_CHECK(c, near(things->get<AnimPlayer>(t5)->time, 0.0f) && near(things->get<AnimPlayer>(t6)->time, 0.2f));
    Handle t7 = things->spawn(transform2_at(point2{0, 0}), Sprite{}, anim_player(Handle{}));
    run(0.1);
    KV_CHECK(c, near(things->get<AnimPlayer>(t7)->time, 0.0f) && anim_stats(e)->players == 3 && anim_stats(e)->advanced == 1);
    things->destroy(t5); things->destroy(t6); things->destroy(t7);

    // ---- the file format -----------------------------------------------------------
    const char* text =
        "{ \"loop\": \"pingpong\", \"tracks\": ["
        "  { \"channel\": \"uv\", \"grid\": [2, 2], \"fps\": 10, \"frames\": [0, 3] },"
        "  { \"channel\": \"alpha\", \"interp\": \"linear\", \"keys\": [[0, 1], [0.2, 0]] },"
        "  { \"channel\": \"position\", \"keys\": [[0, [0, 0]], [0.1, [0, 10]]] },"
        "  { \"channel\": \"event\", \"keys\": [[0.1, \"step\"]] } ] }";
    AnimClip parsed;
    const char* why = nullptr;
    u32 at = 0;
    KV_CHECK(c, anim_parse(text, strlen(text), mem_general(), parsed, &why, &at));
    KV_CHECK(c, parsed.loop == AnimLoop::PingPong && near(parsed.duration, 0.2f) && parsed.tracks.len == 4);
    KV_CHECK(c, parsed.keys.len == 2 + 2 + 2 + 1);
    KV_CHECK(c, near(parsed.keys[1].time, 0.1f) && near(parsed.keys[1].v[0], 0.5f) && near(parsed.keys[1].v[1], 0.5f) && near(parsed.keys[1].v[2], 0.5f));
    KV_CHECK(c, anim_sample(parsed, parsed.tracks[1], 0.1f, v) && near(v[0], 0.5f)); // linear alpha midpoint
    KV_CHECK(c, parsed.keys[6].event == name_intern("step"));
    parsed.tracks.free(); parsed.keys.free();
    const char* bad_texts[] = {
        "[]",
        "{ \"tracks\": [ { \"channel\": \"spin\", \"keys\": [] } ] }",
        "{ \"tracks\": [ { \"channel\": \"uv\", \"interp\": \"linear\", \"keys\": [[0, [0, 0, 1, 1]]] } ] }",
        "{ \"tracks\": [ { \"channel\": \"tint\", \"keys\": [[0, [1, 1]]] } ] }",
        "{ \"duration\": 0.5, \"tracks\": [ { \"channel\": \"alpha\", \"keys\": [[1, 0]] } ] }",
        "{ \"tracks\": [ { \"channel\": \"uv\", \"grid\": [2, 1], \"fps\": 10, \"frames\": [2] } ] }",
    };
    for (const char* b : bad_texts) {
        AnimClip x;
        KV_CHECK(c, !anim_parse(b, strlen(b), mem_general(), x, &why, &at) && why);
        x.tracks.free(); x.keys.free();
    }

    // ---- a position track on a child drives its Local2; the scene composes after ----
    static const AnimKey pos[] = {{.time = 0.0f, .v = {0, 0}}, {.time = 1.0f, .v = {10, 0}}};
    const AnimTrackDesc slide_tracks[] = {{.channel = AnimChannel::Position, .interp = AnimInterp::Linear, .keys = pos}};
    AnimClipDesc slide_d;
    slide_d.name = "slide"; slide_d.loop = AnimLoop::Once; slide_d.tracks = slide_tracks;
    Handle slide = anim_create(e, slide_d);
    SpriteWorld& roots = *sprite_world(e);
    Handle root = roots.create(transform2_at(point2{100, 0}), Sprite{});
    Handle kid = kids->spawn(Transform2{}, Sprite{}, local2_at(point2{0, 0}), Parent{root}, anim_player(slide));
    run(0.5);
    KV_CHECK(c, near(kids->get<Local2>(kid)->position.x, 5.0f) && near(kids->get<Transform2>(kid)->position.x, 105.0f));

    // ---- names: the descriptor's, or the content's ------------------------------------
    KV_CHECK(c, anim_find(e, "slide") == slide && strcmp(handle_name(slide), "slide") == 0);
    AnimClipDesc anon = slide_d;
    anon.name = "";
    Handle a1 = anim_create(e, anon), a2 = anim_create(e, anon);
    KV_CHECK(c, a1 != a2 && !strcmp(anim_clip(e, a1)->path, anim_clip(e, a2)->path) && !strncmp(anim_clip(e, a1)->path, "clip:", 5));
    KV_CHECK(c, anim_find(e, anim_clip(e, a1)->path) == a1); // the first keeps the name

    // ---- write, then parse: the same clip, key for key ---------------------------------
    {
        static const AnimKey tint[] = {{.time = 0.0f, .v = {1, 0.5f, 0.25f, 1}}, {.time = 0.3f, .v = {0, 0, 0, 0.125f}}};
        static const AnimKey al[] = {{.time = 0.1f, .v = {0.75f}}};
        static const AnimKey sc[] = {{.time = 0.0f, .v = {1, 1}}, {.time = 0.7f, .v = {2.5f, 0.5f}}};
        AnimKey evk[1];
        evk[0].time = 0.2f; evk[0].event = name_intern("ping");
        const AnimTrackDesc all[] = {
            {.channel = AnimChannel::Tint, .interp = AnimInterp::Linear, .keys = tint},
            {.channel = AnimChannel::Alpha, .keys = al},
            {.channel = AnimChannel::Scale, .interp = AnimInterp::Linear, .keys = sc},
            {.channel = AnimChannel::Event, .keys = evk},
        };
        for (u32 lm = 0; lm < 3; ++lm) {
            AnimClipDesc d;
            d.name = "every"; d.duration = 0.8f; d.loop = (AnimLoop)lm; d.tracks = all;
            Handle h = anim_create(e, d, Handle{});
            asset_ref(e, h);
            const AnimClip* orig = anim_clip(e, h);
            const AnimClip* flip = anim_clip(e, loop);
            const AnimClip* both[] = {orig, flip};
            for (const AnimClip* src : both) {
                SaveWriter w;
                w.init(mem_general());
                KV_CHECK(c, anim_write(*src, w) && w.finish());
                AnimClip back;
                const char* werr = nullptr;
                u32 wat = 0;
                bool ok = anim_parse((const char*)w.out.data, w.out.len, mem_general(), back, &werr, &wat);
                KV_CHECK(c, ok);
                bool same = ok && back.duration == src->duration && back.loop == src->loop && back.tracks.len == src->tracks.len && back.keys.len == src->keys.len;
                for (u32 t = 0; same && t < back.tracks.len; ++t)
                    same = back.tracks[t].channel == src->tracks[t].channel && back.tracks[t].interp == src->tracks[t].interp && back.tracks[t].count == src->tracks[t].count;
                for (u32 k = 0; same && k < back.keys.len; ++k)
                    same = back.keys[k].time == src->keys[k].time && !memcmp(back.keys[k].v, src->keys[k].v, sizeof back.keys[k].v) && back.keys[k].event == src->keys[k].event;
                KV_CHECK(c, same);
                back.tracks.free(); back.keys.free();
                w.free();
            }
            asset_unref(e, h);
        }
    }

    // ---- scopes: a clip lives while a scope holds it -----------------------------------
    {
        Handle scope = asset_scope_create(e, "clips");
        AnimClipDesc d = slide_d;
        d.name = "scoped";
        Handle h = anim_create(e, d, scope);
        KV_CHECK(c, anim_clip(e, h) && asset_refs(e, h) == 1);
        asset_scope_release(e, scope);
        run(0.0);
        KV_CHECK(c, anim_clip(e, h) == nullptr && anim_find(e, "scoped").is_null());
    }

    // ---- sheets: the format ------------------------------------------------------------
    {
        const char* sheet = R"({ "texture": "t/hero.png", "cell": [16, 16], "fps": 4,
            "clips": { "walk.down": { "column": 1, "rows": [0, 1, 2, 3] },
                       "idle": { "cells": [[0, 0]] },
                       "spin": { "row": 2, "columns": [3, 2], "fps": 2, "loop": "pingpong" },
                       "hit":  { "cells": [[0, 4]], "duration": 0.3, "loop": "once", "events": [[0.1, "hit"]] } },
            "sets": { "walk": { "down": "walk.down" } } })";
        AnimSheetParsed p;
        why = nullptr;
        MemScope scope;
        KV_CHECK(c, anim_sheet_parse(sheet, strlen(sheet), 64, 112, mem_scratch(), p, &why));
        KV_CHECK(c, !strcmp(p.texture, "t/hero.png") && p.cols == 4 && p.rows == 7 && p.cell_size.x == 16 && p.clips.len == 4 && p.sets.len == 1);
        const AnimClipDesc& wd = p.clips[0];
        KV_CHECK(c, !strcmp(wd.name, "walk.down") && wd.loop == AnimLoop::Loop && near(wd.duration, 1.0f) && wd.tracks.len == 1 && wd.tracks[0].keys.len == 4);
        KV_CHECK(c, near(wd.tracks[0].keys[2].time, 0.5f) && near(wd.tracks[0].keys[2].v[0], 0.25f) && near(wd.tracks[0].keys[2].v[1], 2.0f / 7.0f) && near(wd.tracks[0].keys[2].v[2], 0.25f));
        const AnimClipDesc& sp = p.clips[2];
        KV_CHECK(c, sp.loop == AnimLoop::PingPong && near(sp.duration, 1.0f) && near(sp.tracks[0].keys[0].v[0], 0.75f));
        const AnimClipDesc& hit = p.clips[3];
        KV_CHECK(c, hit.loop == AnimLoop::Once && near(hit.duration, 0.3f) && hit.tracks.len == 2 && hit.tracks[1].keys[0].event == name_intern("hit"));
        KV_CHECK(c, p.sets[0].set == name_intern("walk") && p.sets[0].clip.bits == 0);
        // Each built as a clip.
        for (const AnimClipDesc& d : p.clips) {
            AnimClip built;
            KV_CHECK(c, anim_build(d, mem_general(), built, &why));
            built.tracks.free(); built.keys.free();
        }
        const char* bad_sheets[] = {
            R"({ "cell": [16, 16], "clips": {} })",
            R"({ "texture": "x.png", "clips": {} })",
            R"({ "texture": "x.png", "cell": [16, 16], "clips": { "a": { "cells": [[9, 0]] } } })",
            R"({ "texture": "x.png", "cell": [16, 16], "clips": { "a": { "column": 0 } } })",
            R"({ "texture": "x.png", "cell": [16, 16], "clips": { "a": { "cells": [[0, 0]] } }, "sets": { "s": { "v": "nope" } } })",
        };
        for (const char* b : bad_sheets) {
            AnimSheetParsed q;
            why = nullptr;
            KV_CHECK(c, !anim_sheet_parse(b, strlen(b), 64, 112, mem_scratch(), q, &why) && why);
        }
    }

    engine_shutdown(e);
}
