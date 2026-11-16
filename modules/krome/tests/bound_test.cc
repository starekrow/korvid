// What widgets read and draw (docs/PRDs/Krome-Slice-4-PRD.md, milestone 1): properties bound
// to the model and set again when it changes, `@key=value` bindings, percent sizes, scroll
// boxes with bars (dragged, paged, both ways, Shift+wheel), tooltips after a rest, and the
// right button's context event.
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

const char* k_screen = R"lua(
return {
  model = { left = 120, fade = 0.5, tint = "#ff0000", done = "40%", mode = "fast", ctx = "", cx = 0, cy = 0 },
  on = {
    ctx = function(id, x, y) model.ctx = id; model.cx = x; model.cy = y end,
  },
  root = col { pad = 10, gap = 10,
    row { id = "top", gap = 10, h = 40,
      box { id = "side", w = "@left", h = 40, bg = "@tint", opacity = "@fade" },
      box { id = "track", w = 200, h = 10, bg = "#202020",
        box { id = "bar", w = "@done", h = 10, bg = "#2f6fed" } },
    },
    row { gap = 10, w = "fill",
      hit { id = "fast", bind = "mode=fast", w = 60, h = 20, bg = "#333333", selected = { bg = "#00ff00" } },
      hit { id = "slow", bind = "mode=slow", w = 60, h = 20, bg = "#333333", selected = { bg = "#00ff00" } },
      box { id = "half", w = "50%", h = 20, bg = "#444444" },
    },
    col { id = "list", scroll = true, w = 200, h = 100, bg = "#111111", tooltip = "The list",
      box { id = "i1", w = 200, h = 100, bg = "#222222", context = "ctx" },
      box { id = "i2", w = 200, h = 100, bg = "#333333" },
      box { id = "i3", w = 200, h = 100, bg = "#444444" },
      box { id = "i4", w = 200, h = 100, bg = "#555555" },
    },
    col { id = "wide", scroll = "xy", w = 200, h = 100, bg = "#111111",
      box { id = "w1", w = 600, h = 80, bg = "#222222" },
      box { id = "w2", w = 600, h = 80, bg = "#333333" },
    },
  },
}
)lua";

// The same `@key=value` as markup writes it: the key and the value apart.
const char* k_doc = R"json({
  "kim": 1, "file": "kv/eq.kim", "scripts": ["kv/eq.lua"],
  "screens": { "main": { "w": "box", "p": { "dir": "col" }, "c": [
    { "w": "hit", "id": "a", "b": "pick", "bv": "a", "p": { "w": 10, "h": 10 } },
    { "w": "hit", "id": "b", "b": "pick", "bv": "b", "p": { "w": 10, "h": 10 } },
    { "w": "box", "id": "sized", "p": { "w": "@size", "h": 10 } }
  ] } }
})json";
const char* k_doc_script = "return { model = { pick = 'b', size = 30 }, on = {} }";

void frames(Engine& e, u32 n) {
    for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, 1.0 / 60.0); engine_frame_cycle(e, 1.0 / 60.0); }
}

const KromeNode& node_of(const KromeTree& t, const char* id) {
    static KromeNode none;
    u32 n = krome_find(t, name_intern(id));
    return n == KROME_NONE ? none : t.nodes[n];
}

bool rect_is(rect2 r, f32 x, f32 y, f32 w, f32 h) {
    if (r.x == x && r.y == y && r.w == w && r.h == h) return true;
    KV_ERROR("test", "rect (%g, %g, %g, %g), wanted (%g, %g, %g, %g)", r.x, r.y, r.w, r.h, x, y, w, h);
    return false;
}

u32 rgba(color c) {
    auto b = [](f32 v) { return (u32)(v * 255.0f + 0.5f); };
    return b(c.r) << 24 | b(c.g) << 16 | b(c.b) << 8 | b(c.a);
}

// Overlays a screen has open, and of those the passive ones (tooltips).
u32 overlays(KromeScreen& sc, bool passive) {
    u32 n = 0;
    for (KromeOverlay* o : sc.overlays) if (o->passive == passive) ++n;
    return n;
}

} // namespace

void test_bound(kv::TestCtx& c) {
    using namespace kv;
    c.begin("krome bound");
    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KromeModule& s = *krome_state(e);
    krome_viewport(e, 800, 600, 1.0f);
    Name form = krome_open_source(e, "kv/bound", k_screen);
    frames(e, 2);
    KV_CHECK(c, krome_ready(e, form));
    KromeScreen& sc = *krome_screen(s, form);
    const KromeTree& t = sc.tree;
    KV_CHECK(c, !krome_errors(e) || krome_errors(e)->count == 0);

    // Bound properties: set from the model, and again when it changes, with no rebuild.
    KV_CHECK(c, rect_is(node_of(t, "side").rect, 10, 10, 120, 40));
    KV_CHECK(c, rgba(t.looks[node_of(t, "side").look].bg) == 0xFF0000FFu && t.looks[node_of(t, "side").look].opacity == 0.5f);
    // A percentage of the parent's inside.
    KV_CHECK(c, rect_is(node_of(t, "bar").rect, 140, 10, 80, 10));
    KV_CHECK(c, node_of(t, "half").rect.w == 390.0f); // half of 780
    u64 rebuilt = s.rebuilt_nodes;
    krome_set(e, form, "left", 200.0);
    krome_set(e, form, "done", "75%");
    krome_set(e, form, "tint", "#00ff00");
    frames(e, 2);
    KV_CHECK(c, node_of(t, "side").rect.w == 200.0f && node_of(t, "bar").rect.w == 150.0f);
    KV_CHECK(c, rgba(t.looks[node_of(t, "side").look].bg) == 0x00FF00FFu);
    KV_CHECK(c, s.rebuilt_nodes == rebuilt);
    // A value that does not parse for the property is its error.
    krome_clear_errors(e);
    krome_set(e, form, "left", "wide");
    frames(e, 1);
    KV_CHECK(c, krome_errors(e) && krome_errors(e)->count == 1 && node_of(t, "side").rect.w == 200.0f);
    krome_clear_errors(e);

    // `@key=value`: selected while the model holds the value; a press writes it.
    KV_CHECK(c, (node_of(t, "fast").flags & KROME_SELECTED) && !(node_of(t, "slow").flags & KROME_SELECTED));
    KV_CHECK(c, krome_press(e, form, "slow"));
    frames(e, 1);
    KV_CHECK(c, !strcmp(krome_model_text(e, form, "mode"), "slow"));
    KV_CHECK(c, !(node_of(t, "fast").flags & KROME_SELECTED) && (node_of(t, "slow").flags & KROME_SELECTED));
    krome_set(e, form, "mode", "fast");
    frames(e, 1);
    KV_CHECK(c, (node_of(t, "fast").flags & KROME_SELECTED) && !(node_of(t, "slow").flags & KROME_SELECTED));

    // A scroll box's bar: a thumb a quarter of the track (100 of 400 shown), at its top.
    const rect2 lr = node_of(t, "list").rect; // (10, 90, 200, 100)
    KV_CHECK(c, rect_is(lr, 10, 90, 200, 100));
    f32 bx = lr.x + lr.w - 3.0f; // on the bar, 6 wide at the right edge
    // A press on the track below the thumb pages a view down; the keyboard stays put.
    krome_pointer(s, bx, lr.y + 80.0f, true, 0, false);
    krome_pointer(s, bx, lr.y + 80.0f, false, 0, false);
    frames(e, 1);
    KV_CHECK(c, node_of(t, "list").scroll.y == 100.0f && s.captured.node == KROME_NONE);
    // The thumb (now at 25..50) dragged down by 25 pixels moves the content 100.
    krome_pointer(s, bx, lr.y + 30.0f, true, 0, false);
    KV_CHECK(c, s.bar.active && !s.bar.horizontal && s.bar.grab == 5.0f);
    // (Released before the frames: a frame's pointer is the window's, which a test has not.)
    krome_pointer(s, bx, lr.y + 55.0f, true, 0, false);
    KV_CHECK(c, node_of(t, "list").scroll.y == 200.0f);
    krome_pointer(s, bx, lr.y + 55.0f, false, 0, false);
    frames(e, 1);
    KV_CHECK(c, node_of(t, "list").scroll.y == 200.0f);
    krome_pointer(s, bx, lr.y + 55.0f, true, 0, false);
    // Past the end it stops at the end.
    krome_pointer(s, bx, lr.y + 500.0f, true, 0, false);
    krome_pointer(s, bx, lr.y + 500.0f, false, 0, false);
    frames(e, 1);
    KV_CHECK(c, node_of(t, "list").scroll.y == 300.0f && !s.bar.active);
    {   // It is painted: a box in the bar's place, after the rows.
        UiList l;
        l.init(mem_general());
        krome_paint_screen(s, sc, l);
        // The thumb sits at 75..100 of the track, inset a pixel: (205, 165) 4 by 25.
        bool thumb = false;
        for (u32 i = 0; i < l.count(); ++i) {
            const u32* w = &l.words[l.offsets[i]];
            if ((w[0] & 3) != 0) continue; // a box
            f32 r[4];
            memcpy(r, w + 6, sizeof r);
            if (r[0] == 205.0f && r[1] == 165.0f && r[2] == 4.0f && r[3] == 25.0f) thumb = true;
        }
        KV_CHECK(c, thumb);
        l.free();
    }

    // Both ways: the wheel goes down, Shift's (dx) across; each held to its content.
    const rect2 wr = node_of(t, "wide").rect;
    krome_pointer(s, wr.x + 50.0f, wr.y + 50.0f, false, 0, false);
    krome_pointer(s, wr.x + 50.0f, wr.y + 50.0f, false, -1.0f, false);
    krome_wheel(s, 10.0f, 0.0f);
    frames(e, 1);
    KV_CHECK(c, node_of(t, "wide").scroll.y == 40.0f && node_of(t, "wide").scroll.x == 400.0f);
    KV_CHECK(c, node_of(t, "w1").rect.x == wr.x - 400.0f && node_of(t, "w1").rect.w == 600.0f);

    // A tooltip: after the pointer rests, an overlay the pointer goes through; gone when it
    // leaves, and shut by a press until it does.
    s.tip_delay = 0.0;
    krome_pointer(s, 600, 500, false, 0, false);
    krome_pointer(s, lr.x + 50.0f, lr.y + 50.0f, false, 0, false);
    KV_CHECK(c, overlays(sc, true) == 1 && s.under.overlay == 0);
    krome_view_frame(s); // placed, without a frame's pointer moving it away
    KromeOverlay* tip = sc.overlays.len ? sc.overlays.last() : nullptr;
    KV_CHECK(c, tip && tip->tree.root != KROME_NONE && tip->tree.nodes[tip->tree.root].rect.x == lr.x + 62.0f);
    krome_pointer(s, lr.x + 50.0f, lr.y + 50.0f, true, 0, false);
    krome_pointer(s, lr.x + 50.0f, lr.y + 50.0f, false, 0, false);
    krome_pointer(s, lr.x + 51.0f, lr.y + 50.0f, false, 0, false);
    KV_CHECK(c, overlays(sc, true) == 0);
    krome_pointer(s, 600, 500, false, 0, false);
    krome_pointer(s, lr.x + 50.0f, lr.y + 50.0f, false, 0, false);
    KV_CHECK(c, overlays(sc, true) == 1);
    krome_pointer(s, 600, 500, false, 0, false);
    KV_CHECK(c, overlays(sc, true) == 0);
    s.tip_delay = 60.0;
    krome_pointer(s, lr.x + 50.0f, lr.y + 50.0f, false, 0, false);
    KV_CHECK(c, overlays(sc, true) == 0); // not rested long enough

    // The right button: the nearest context event, with where it was.
    krome_set(e, form, "left", 120.0);
    frames(e, 1);
    node_of(t, "list"); // scrolled to the end: i4 shows; scroll back to i1
    const_cast<KromeNode&>(node_of(t, "list")).scroll.y = 0.0f;
    const_cast<KromeTree&>(t).dirty = true;
    frames(e, 1);
    krome_context_press(s, lr.x + 20.0f, lr.y + 30.0f);
    KV_CHECK(c, !strcmp(krome_model_text(e, form, "ctx"), "i1"));
    KV_CHECK(c, !strcmp(krome_model_text(e, form, "cx"), "30") && !strcmp(krome_model_text(e, form, "cy"), "120"));
    krome_context_press(s, 700, 20); // nothing there with one
    KV_CHECK(c, s.fired > 0);

    // Markup's form of `@key=value` and of a bound property.
    {
        JsonDoc doc;
        KV_CHECK(c, json_parse(k_doc, strlen(k_doc), mem_scratch(), doc));
        Array<u8> k{};
        kbor_write(doc, k);
        krome_provide(e, "kv/eq.kimb", k.data, k.len);
        k.free();
        krome_provide(e, "kv/eq.lua", k_doc_script, strlen(k_doc_script));
        Name eq = krome_open(e, "kv/eq.kimb", "main");
        frames(e, 3);
        KV_CHECK(c, krome_ready(e, eq));
        const KromeTree* et = krome_tree(e, eq);
        KV_CHECK(c, et && !(node_of(*et, "a").flags & KROME_SELECTED) && (node_of(*et, "b").flags & KROME_SELECTED));
        KV_CHECK(c, et && node_of(*et, "sized").rect.w == 30.0f);
        KV_CHECK(c, krome_press(e, eq, "a"));
        frames(e, 1);
        KV_CHECK(c, !strcmp(krome_model_text(e, eq, "pick"), "a") && (node_of(*et, "a").flags & KROME_SELECTED));
    }
    engine_shutdown(e);
}
