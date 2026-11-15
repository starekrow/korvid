// Krome's view without a window: laid out against a fixed viewport, painted into a list,
// the pointer driven by hand. Hover and press looks, clicks, the wheel, overlays anchored
// and kept in the window, light ones closed by a press outside (which goes no further),
// modals and Escape, a scale of 2, and a still screen costing nothing.
#include "korvid/krome/krome.h"
#include "../src/internal.h"
#include "korvid/asset/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/module.h"
#include <string.h>

namespace {

using namespace kv;

const char* k_screen = R"lua(
return {
  model = { saved = 0, picked = "", oks = 0 },
  on = {
    save  = function() model.saved = model.saved + 1 end,
    menu  = function()
      open_overlay(col { id = "menu_list", bg = "#101010", pad = 4,
                         hit { id = "pick_a", event = "pick", w = 80, h = 20 },
                         hit { id = "pick_b", event = "pick", w = 80, h = 20 } }, { anchor = "menu" })
    end,
    low   = function() open_overlay(box { id = "low_list", w = 50, h = 100, bg = "#202020" }, { anchor = "low" }) end,
    pick  = function(id) model.picked = id; close_overlay() end,
    modal = function()
      open_overlay(box { id = "dialog", w = 200, h = 100, bg = "#303030",
                         hit { id = "ok", event = "ok", w = 40, h = 20 } }, { modal = true, dim = "#00000080" })
    end,
    ok    = function() model.oks = model.oks + 1; close_overlay() end,
  },
  root = col { pad = 10, gap = 10,
    row { gap = 10,
      hit { id = "save", event = "save", w = 100, h = 30, bg = "#334455",
            hover = { bg = "#ff0000" }, pressed = { bg = "#00ff00" } },
      hit { id = "menu", event = "menu", w = 100, h = 30, bg = "#334455" },
      hit { id = "modal", event = "modal", w = 100, h = 30, bg = "#334455" },
      hit { id = "off", event = "save", w = 100, h = 30, bg = "#334455", disabled = true },
    },
    col { id = "list", scroll = true, w = 200, h = 100, bg = "#111111",
      box { id = "i1", w = 200, h = 60, bg = "#222222" },
      box { id = "i2", w = 200, h = 60, bg = "#333333" },
      box { id = "i3", w = 200, h = 60, bg = "#444444" },
    },
    spacer {},
    hit { id = "low", event = "low", w = 100, h = 30, bg = "#334455" },
  },
}
)lua";

void frames(Engine& e, u32 n) {
    for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, 1.0 / 60.0); }
}

rect2 rect_of(const KromeTree& t, const char* id) {
    u32 n = krome_find(t, name_intern(id));
    return n == KROME_NONE ? rect2{-1, -1, 0, 0} : t.nodes[n].rect;
}

bool rect_is(rect2 r, f32 x, f32 y, f32 w, f32 h) {
    if (r.x == x && r.y == y && r.w == w && r.h == h) return true;
    KV_ERROR("test", "rect (%g, %g, %g, %g), wanted (%g, %g, %g, %g)", r.x, r.y, r.w, r.h, x, y, w, h);
    return false;
}

// The fill of the first box the screen paints (the save button's).
u32 first_fill(KromeModule& s, KromeScreen& sc) {
    UiList l;
    l.init(mem_general());
    krome_paint_screen(s, sc, l);
    u32 fill = l.count() ? l.words[l.offsets[0] + 2] : 0;
    l.free();
    return fill;
}

} // namespace

void test_view(kv::TestCtx& c) {
    using namespace kv;
    c.begin("krome view");
    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KromeModule& s = *krome_state(e);
    krome_viewport(e, 800, 600, 1.0f);
    Name form = krome_open_source(e, "kv/view", k_screen);
    frames(e, 2);
    KV_CHECK(c, krome_ready(e, form));
    KromeScreen& sc = *krome_screen(s, form);
    const KromeTree& t = sc.tree;
    KV_CHECK(c, rect_is(rect_of(t, "save"), 10, 10, 100, 30) && rect_is(rect_of(t, "menu"), 120, 10, 100, 30));
    KV_CHECK(c, rect_is(rect_of(t, "list"), 10, 50, 200, 100) && rect_is(rect_of(t, "low"), 10, 560, 100, 30));

    // Still: no layout and no paint while nothing changes.
    KromeStats before = krome_stats(e);
    KV_CHECK(c, before.layouts >= 1 && before.paints >= 1);
    frames(e, 60);
    KromeStats still = krome_stats(e);
    KV_CHECK(c, still.layouts == before.layouts && still.paints == before.paints);

    // Hover, press, release: the looks follow, and the release over it is a click.
    KV_CHECK(c, first_fill(s, sc) == 0xFF554433u);
    krome_pointer(s, 50, 20, false, 0, false);
    KV_CHECK(c, s.under.node == krome_find(t, name_intern("save")) && first_fill(s, sc) == 0xFF0000FFu);
    frames(e, 1);
    KV_CHECK(c, krome_stats(e).paints == still.paints + 1 && krome_stats(e).layouts == still.layouts);
    krome_pointer(s, 50, 20, true, 0, false);
    KV_CHECK(c, first_fill(s, sc) == 0xFF00FF00u);
    krome_pointer(s, 60, 22, false, 0, false);
    KV_CHECK(c, strcmp(krome_model_text(e, form, "saved"), "1") == 0 && first_fill(s, sc) == 0xFF0000FFu);
    // A press that leaves before its release is not a click; pressed shows only while over it.
    krome_pointer(s, 50, 20, true, 0, false);
    krome_pointer(s, 500, 300, true, 0, false);
    KV_CHECK(c, first_fill(s, sc) == 0xFF554433u);
    krome_pointer(s, 500, 300, false, 0, false);
    KV_CHECK(c, strcmp(krome_model_text(e, form, "saved"), "1") == 0);
    // Disabled takes no press.
    krome_pointer(s, 350, 20, true, 0, false);
    krome_pointer(s, 350, 20, false, 0, false);
    KV_CHECK(c, strcmp(krome_model_text(e, form, "saved"), "1") == 0);

    // The wheel scrolls the box under the pointer, inside its content.
    krome_pointer(s, 100, 100, false, -1.0f, false);
    frames(e, 1);
    KV_CHECK(c, t.nodes[krome_find(t, name_intern("list"))].scroll.y == 40.0f && rect_is(rect_of(t, "i1"), 10, 10, 200, 60));
    krome_pointer(s, 100, 100, false, -5.0f, false);
    frames(e, 1);
    KV_CHECK(c, t.nodes[krome_find(t, name_intern("list"))].scroll.y == 80.0f);
    {   // What is in the scroll box is clipped to it.
        UiList l;
        l.init(mem_general());
        krome_paint_screen(s, sc, l);
        u32 clipped = 0;
        for (u32 i = 0; i < l.count(); ++i) if ((l.words[l.offsets[i]] >> 2 & 0xFFFF) == 1) ++clipped;
        KV_CHECK(c, l.clips.len == 8 && clipped == 3 && l.clips[4] == 10.0f && l.clips[5] == 50.0f);
        l.free();
    }

    // A menu: below its button, over everything; a pick runs its handler and closes it.
    krome_pointer(s, 150, 20, true, 0, false);
    krome_pointer(s, 150, 20, false, 0, false);
    frames(e, 1);
    KV_CHECK(c, sc.overlays.len == 1);
    const KromeTree& menu = sc.overlays[0]->tree;
    KV_CHECK(c, rect_is(rect_of(menu, "menu_list"), 120, 40, 88, 48) && rect_is(rect_of(menu, "pick_b"), 124, 64, 80, 20));
    krome_pointer(s, 130, 70, false, 0, false);
    KV_CHECK(c, s.under.overlay == sc.overlays[0]->id);
    krome_pointer(s, 130, 70, true, 0, false);
    krome_pointer(s, 130, 70, false, 0, false);
    frames(e, 1);
    KV_CHECK(c, sc.overlays.len == 0 && strcmp(krome_model_text(e, form, "picked"), "pick_b") == 0);
    // Opened again, a press outside closes it and reaches nothing: the save under it is not pressed.
    krome_pointer(s, 150, 20, true, 0, false);
    krome_pointer(s, 150, 20, false, 0, false);
    frames(e, 1);
    KV_CHECK(c, sc.overlays.len == 1);
    krome_pointer(s, 50, 20, true, 0, false);
    krome_pointer(s, 50, 20, false, 0, false);
    frames(e, 1);
    KV_CHECK(c, sc.overlays.len == 0 && strcmp(krome_model_text(e, form, "saved"), "1") == 0);
    // Near the bottom, a menu opens above its button instead.
    krome_pointer(s, 50, 575, true, 0, false);
    krome_pointer(s, 50, 575, false, 0, false);
    frames(e, 1);
    KV_CHECK(c, sc.overlays.len == 1 && rect_is(rect_of(sc.overlays[0]->tree, "low_list"), 10, 460, 50, 100));
    krome_pointer(s, 50, 575, false, 0, true); // Escape
    KV_CHECK(c, sc.overlays.len == 0);
    krome_pointer(s, 50, 575, false, 0, false);

    // A modal: centered, dimming; a press outside does nothing and closes nothing.
    krome_pointer(s, 250, 20, true, 0, false);
    krome_pointer(s, 250, 20, false, 0, false);
    frames(e, 1);
    KV_CHECK(c, sc.overlays.len == 1 && sc.overlays[0]->modal && rect_is(rect_of(sc.overlays[0]->tree, "dialog"), 300, 250, 200, 100));
    krome_pointer(s, 50, 20, true, 0, false);
    krome_pointer(s, 50, 20, false, 0, false);
    KV_CHECK(c, sc.overlays.len == 1 && strcmp(krome_model_text(e, form, "saved"), "1") == 0 && s.under.node == KROME_NONE);
    krome_pointer(s, 310, 260, true, 0, false);
    krome_pointer(s, 310, 260, false, 0, false);
    KV_CHECK(c, sc.overlays.len == 0 && strcmp(krome_model_text(e, form, "oks"), "1") == 0);

    // At a scale of 2 the same layout lands on twice the pixels, and the pointer finds it there.
    krome_viewport(e, 1600, 1200, 2.0f);
    frames(e, 1);
    KV_CHECK(c, rect_is(rect_of(t, "save"), 10, 10, 100, 30));
    krome_pointer(s, 300, 50, false, 0, false);
    KV_CHECK(c, s.under.node == krome_find(t, name_intern("menu")));
    krome_pointer(s, 60, 30, true, 0, false);
    krome_pointer(s, 60, 30, false, 0, false);
    KV_CHECK(c, strcmp(krome_model_text(e, form, "saved"), "2") == 0);
    engine_shutdown(e);
}
