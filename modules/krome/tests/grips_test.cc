// What Krome tells a window with no frame of its own (slice 13, KR41): the rects of the
// nodes with `grip=`, the pressable nodes over them as the program's, an overlay over a
// grip as the program's, and the root's size range as the window's bounds, all in physical
// pixels. A press on a grip is the OS's: Krome takes none.
#include "korvid/krome/krome.h"
#include "../src/internal.h"
#include "korvid/asset/module.h"
#include "korvid/cli/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/module.h"
#include <string.h>

namespace {

using namespace kv;

const char* k_skin = R"lua(
return {
  model = { closes = 0, moves = 0 },
  on = {
    close = function() model.closes = model.closes + 1 end,
    moved = function() model.moves = model.moves + 1 end,
    menu  = function() open_overlay(box { id = "menu_list", w = 120, h = 60, bg = "#101010" }, { anchor = "close" }) end,
  },
  root = stack { w = "275..800", h = "116..600",
    col { w = "fill", h = "fill",
      row { id = "title", grip = "move", event = "moved", h = 20, w = "fill", bg = "#203040",
        spacer {},
        hit { id = "close", event = "close", w = 16, h = 16 },
      },
      hit { id = "menu", event = "menu", w = 40, h = 20 },
    },
    box { id = "corner", grip = "se", w = 12, h = 12, anchor = "bottom-right" },
  },
}
)lua";

void frames(Engine& e, u32 n) {
    for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, 1.0 / 60.0); engine_frame_cycle(e, 1.0 / 60.0); }
}

bool grip_is(const WindowGripRect& g, WindowGrip grip, f32 x, f32 y, f32 w, f32 h) {
    if (g.grip == grip && g.rect.x == x && g.rect.y == y && g.rect.w == w && g.rect.h == h) return true;
    KV_ERROR("test", "grip %u (%g, %g, %g, %g), wanted %u (%g, %g, %g, %g)", (u32)g.grip, g.rect.x, g.rect.y, g.rect.w, g.rect.h,
             (u32)grip, x, y, w, h);
    return false;
}

} // namespace

void test_grips(kv::TestCtx& c) {
    using namespace kv;
    c.begin("krome grips");
    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &cli_module, &asset_module, &krome_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KromeModule& s = *krome_state(e);
    krome_viewport(e, 400, 300, 1.0f);
    Name skin = krome_open_source(e, "kv/skin", k_skin);
    frames(e, 2);
    KV_CHECK(c, krome_ready(e, skin));

    // The title bar moves, the close button inside it is the program's, the corner resizes.
    // The menu button is over no grip: it is left out.
    Array<WindowGripRect> g;
    u32 bounds[4];
    krome_window_parts(s, g, bounds);
    KV_CHECK(c, g.len == 3);
    if (g.len == 3) {
        KV_CHECK(c, grip_is(g[0], WindowGrip::Move, 0, 0, 400, 20));
        KV_CHECK(c, grip_is(g[1], WindowGrip::None, 384, 0, 16, 16));
        KV_CHECK(c, grip_is(g[2], WindowGrip::SE, 388, 288, 12, 12));
    }
    KV_CHECK(c, bounds[0] == 275 && bounds[1] == 116 && bounds[2] == 800 && bounds[3] == 600);

    // A press on the title bar is the OS's: its event does not fire. The close button's does.
    krome_pointer(s, 100, 10, true, 0, false);
    krome_pointer(s, 100, 10, false, 0, false);
    KV_CHECK(c, strcmp(krome_model_text(e, skin, "moves"), "0") == 0);
    krome_pointer(s, 390, 10, true, 0, false);
    krome_pointer(s, 390, 10, false, 0, false);
    KV_CHECK(c, strcmp(krome_model_text(e, skin, "closes"), "1") == 0);

    // An overlay over the title bar is the program's where it is.
    krome_pointer(s, 20, 30, true, 0, false);
    krome_pointer(s, 20, 30, false, 0, false);
    frames(e, 1);
    krome_window_parts(s, g, bounds);
    bool covered = false;
    for (const WindowGripRect& r : g) if (r.grip == WindowGrip::None && r.rect.w == 120 && r.rect.h == 60) covered = true;
    KV_CHECK(c, covered);

    // At a scale of 2, twice the pixels, and the bounds with them.
    krome_viewport(e, 800, 600, 2.0f);
    frames(e, 1);
    krome_window_parts(s, g, bounds);
    KV_CHECK(c, g.len >= 1 && grip_is(g[0], WindowGrip::Move, 0, 0, 800, 40));
    KV_CHECK(c, bounds[0] == 550 && bounds[1] == 232 && bounds[2] == 1600 && bounds[3] == 1200);

    // grip=drag (KR42): the program's own move where it may place a window, so the band is no
    // grip (and with none, the list is empty: the window's input is its own) and Krome takes
    // its press; the OS's move where it may not.
    // The travel is the pointer's on the desktop (set here as the platform would), and a
    // drag is no click.
    krome_close(e, skin);
    krome_viewport(e, 400, 300, 1.0f);
    Name drag = krome_open_source(e, "kv/drag", R"lua(
return {
  model = { clicks = 0 },
  on = { clicked = function() model.clicks = model.clicks + 1 end },
  root = col { w = "fill", h = "fill",
    row { id = "band", grip = "drag", event = "clicked", h = 20, w = "fill", bg = "#203040" },
  },
}
)lua");
    frames(e, 2);
    KV_CHECK(c, krome_ready(e, drag));
    bool place = platform_can_place(e);
    krome_window_parts(s, g, bounds);
    KV_CHECK(c, place ? g.len == 0 : g.len == 1 && grip_is(g[0], WindowGrip::Move, 0, 0, 400, 20));
    if (place) {
        s.desk_ok = true;
        s.desk_x = 500.0f;
        s.desk_y = 300.0f;
        krome_pointer(s, 100, 10, true, 0, false);
        KV_CHECK(c, s.drag.on && s.drag.dx == 0.0f && s.drag.dy == 0.0f);
        s.desk_x = 530.0f;
        s.desk_y = 290.0f;
        krome_pointer(s, 130, 0, true, 0, false);
        KV_CHECK(c, s.drag.on && s.drag.dx == 30.0f && s.drag.dy == -10.0f);
        krome_pointer(s, 130, 0, false, 0, false);
        KV_CHECK(c, !s.drag.on);
        KV_CHECK(c, strcmp(krome_model_text(e, drag, "clicks"), "0") == 0);
        s.desk_ok = false;
    }
    g.free();
    engine_shutdown(e);
}
