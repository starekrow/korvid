// A screen built whole again under the pointer (the dead nodes of rebuilt regions past
// the live ones): never in the middle of a handler, so a press whose handler rebuilds the
// list it is in, again and again, is safe; a scroll box with no id keeps its place; a
// scrollbar being dragged goes on through it.
#include "korvid/krome/krome.h"
#include "../src/internal.h"
#include "korvid/asset/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/kbor.h"
#include "korvid/core/test.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/module.h"
#include <string.h>

namespace {

using namespace kv;

const char* k_screen = R"({"kim": 1, "file": "r/s.kim", "scripts": ["r/s.lua"],
  "screens": { "main": { "w": "box", "p": { "dir": "col", "pad": 10, "gap": 10 }, "c": [
    { "w": "box", "f": ["scroll"], "p": { "dir": "col", "w": 300, "h": 240 }, "c": [
      { "w": "each", "v": "r", "in": "rows", "height": 24, "c": [
        { "w": "hit", "id": "{r.id}", "e": "bump", "p": { "dir": "row", "h": 24, "w": 280 }, "c": [
          { "w": "box", "p": { "w": 8, "h": 8 } },
          { "w": "text", "t": "{r.name}" },
          { "w": "text", "t": "{r.id}" } ] } ] } ] }
  ] } } })";
const char* k_script = R"lua(
local rows = {}
for i = 1, 2000 do rows[i] = { id = "r" .. i, name = "row " .. i } end
return { model = { rows = rows, bumps = 0 }, on = {
  bump = function(id) model.bumps = model.bumps + 1; model.rows = rows end,
} }
)lua";

void provide_json(Engine& e, const char* path, const char* json) {
    JsonDoc doc;
    if (!json_parse(json, strlen(json), mem_scratch(), doc)) { KV_ERROR("test", "%s: %s at line %u", path, doc.error, doc.error_line); return; }
    Array<u8> k{};
    kbor_write(doc, k);
    krome_provide(e, path, k.data, k.len);
    k.free();
}

void frames(Engine& e, u32 n) {
    for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, 1.0 / 60.0); engine_frame_cycle(e, 1.0 / 60.0); }
}

u32 scroll_box(const KromeTree& t) {
    for (u32 i = 0; i < t.nodes.len; ++i) if ((t.nodes[i].flags & KROME_SCROLL) && !(t.nodes[i].flags & KROME_DEAD)) return i;
    return KROME_NONE;
}

} // namespace

void test_rebuild(kv::TestCtx& c) {
    using namespace kv;
    c.begin("krome rebuild");
    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KromeModule& s = *krome_state(e);
    krome_viewport(e, 800, 600, 1.0f);
    provide_json(e, "r/s.kimb", k_screen);
    krome_provide(e, "r/s.lua", k_script, strlen(k_script));
    Name scn = krome_open(e, "r/s.kimb", "main");
    frames(e, 3);
    KV_CHECK(c, krome_ready(e, scn));
    KromeScreen& sc = *krome_screen(s, scn);
    KromeTree& t = sc.tree;

    // Scrolled (a box with no id), then pressed again and again on a row whose handler
    // builds the list again: each press leaves dead nodes, until the screen is built whole.
    u32 box = scroll_box(t);
    t.nodes[box].scroll.y = 2400.0f;
    t.dirty = true;
    frames(e, 2);
    rect2 br = t.nodes[box].rect;
    u32 rebuilds_seen = 0;
    for (u32 i = 0; i < 20; ++i) {
        u32 dead_before = sc.dead;
        input_pointer_override(e, br.x + 40, br.y + 30, 1);
        frames(e, 1);
        input_pointer_override(e, br.x + 40, br.y + 30, 0);
        frames(e, 2);
        if (sc.dead < dead_before) ++rebuilds_seen;
    }
    KV_CHECK(c, !strcmp(krome_model_text(e, scn, "bumps"), "20"));
    KV_CHECK(c, rebuilds_seen >= 1); // the screen was built whole at least once
    box = scroll_box(t);
    KV_CHECK(c, box != KROME_NONE && t.nodes[box].scroll.y == 2400.0f);
    KV_CHECK(c, krome_find(t, name_intern("r101")) != KROME_NONE && t.nodes[krome_find(t, name_intern("r101"))].rect.y == t.nodes[box].rect.y);

    // A long drag on its scrollbar: the list is built again row window by row window, the
    // screen whole now and then, and the drag goes on to where the pointer is.
    br = t.nodes[box].rect;
    f32 bx = br.x + br.w - 3.0f;
    // The thumb (10 rows of 2000 in view: its least length, 20) at 2400 / 47760 of the track's room.
    f32 thumb_y = br.y + 2400.0f / (48000.0f - 240.0f) * (240.0f - 20.0f) + 5.0f;
    input_pointer_override(e, bx, thumb_y, 0);
    frames(e, 1);
    input_pointer_override(e, bx, thumb_y, 1);
    frames(e, 1);
    KV_CHECK(c, s.bar.active);
    u32 full_after = 0;
    for (u32 k = 1; k <= 40; ++k) {
        u32 dead = sc.dead;
        input_pointer_override(e, bx, thumb_y + (f32)k * 5.0f, 1);
        frames(e, 1);
        if (sc.dead < dead) ++full_after;
        if (!s.bar.active) { KV_ERROR("test", "the drag ended at step %u", k); break; }
    }
    KV_CHECK(c, s.bar.active);
    box = scroll_box(t);
    // 200 pixels of 220 of room: most of the way down.
    f32 want = (thumb_y + 200.0f - 5.0f - br.y) / 220.0f * (48000.0f - 240.0f);
    // (The test's pointer reaches Krome a frame late: the last step or the one before it.)
    f32 step = 5.0f / 220.0f * (48000.0f - 240.0f);
    KV_CHECK(c, box != KROME_NONE && t.nodes[box].scroll.y > want - step - 5.0f && t.nodes[box].scroll.y < want + 5.0f);
    input_pointer_override(e, bx, thumb_y + 200.0f, 0);
    frames(e, 1);
    KV_CHECK(c, !s.bar.active);
    // Still, nothing more happens: no rebuild asks for another (the loop that held the mouse).
    {
        u32 dead = sc.dead;
        frames(e, 30);
        KV_CHECK(c, sc.dead == dead && !sc.rebuild);
    }
    // Built whole deep in the list: its rows come back where they were, built once.
    box = scroll_box(t);
    t.nodes[box].scroll.y = 40000.0f;
    t.dirty = true;
    frames(e, 2);
    sc.rebuild = true;
    frames(e, 1);
    u32 after = sc.dead;
    frames(e, 10);
    box = scroll_box(t);
    KV_CHECK(c, after == 0 && sc.dead == 0 && !sc.rebuild && t.nodes[box].scroll.y == 40000.0f);
    {
        const KromeRegion* vr = nullptr;
        for (const KromeRegion& r : sc.regions) if (r.alive && r.height > 0.0f) vr = &r;
        KV_CHECK(c, vr && vr->first > 1600 && vr->first <= 40000 / 24);
    }
    KV_INFO("test", "rebuild: %u whole rebuilds under 20 presses, %u under a 40-step drag", rebuilds_seen, full_after);
    engine_shutdown(e);
}
