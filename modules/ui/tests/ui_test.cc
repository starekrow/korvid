// The UI without a renderer (no glyphs, no pictures; the tree, layout, input and events the
// same): a document read through the mounts; a screen built from it and the same screen
// built in code give the same nodes; anchors, pivots, fit, fill, padding and stacks place
// nodes where they belong; a list moves its focus with the move action (with repeat) and
// emits select and cancel; a reveal runs at its rate, honours pauses, completes on confirm
// and says so; model values feed meters; a scroll clamps; a holding screen holds the sim.
#include "korvid/ui/module.h"
#include "korvid/asset/module.h"
#include "korvid/asset/scope.h"
#include "korvid/audio/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/actions.h"
#include "korvid/platform_hid/module.h"
#include "korvid/sprite/module.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

using namespace kv;

struct Seen { u32 events; u64 last_node, last_event; i32 last_value; u32 covered, uncovered; u64 user; };
Seen g;
void on_ui(TickContext&, Span<const UiEvent> evs) {
    for (const UiEvent& ev : evs) { ++g.events; g.last_node = ev.node; g.last_event = ev.event; g.last_value = ev.value; }
}
void on_covered(TickContext&, Span<const UiCovered> cs) { for (const UiCovered& c : cs) { ++g.covered; g.user = c.user; } }
void on_uncovered(TickContext&, Span<const UiUncovered> cs) { g.uncovered += cs.len; }
constinit const HandlerDesc k_handlers[] = {
    {.name = ".on_ui", KV_ON(UiEvent, on_ui)},
    {.name = ".on_covered", KV_ON(UiCovered, on_covered)},
    {.name = ".on_uncovered", KV_ON(UiUncovered, on_uncovered)},
};
KV_MODULE_DEFINE(k_t) = {.name = "ut", .handlers = k_handlers};

constinit const InputActionDesc k_actions[] = {
    {"move", ActionKind::Axis2, "keys W S A D"},
    {"confirm", ActionKind::Button, "key Enter"},
    {"cancel", ActionKind::Button, "key Backspace"},
};

const char* const k_doc = R"({
  "design": [320, 180], "pixel": true,
  "style": { "font": "text/DejaVuSans.ttf", "size": 8, "colors": { "text": "#ffffff", "gold": "#ffc840" } },
  "screens": {
    "menu": { "type": "panel", "name": "box", "anchor": "center", "size": [100, "fit"], "padding": [4, 6, 4, 6], "fill": "gold",
      "children": [
        { "type": "list", "name": "items", "items": ["Resume", "Save", { "type": "label", "name": "quit", "text": "Quit", "event": "leave" }] },
        { "type": "box", "name": "corner", "anchor": "bottomright", "size": [10, 10], "offset": [-1, -2] } ] },
    "hud": { "type": "stack", "name": "bar", "anchor": "topleft", "dir": "row", "gap": 2, "padding": 3,
      "children": [ { "type": "box", "name": "a", "size": [8, 5] }, { "type": "box", "name": "b", "size": [12, 9] },
                    { "type": "meter", "name": "hp", "style": "bar", "value": "=hp", "max": "=hp_max", "size": [30, 4] } ] },
    "talk": { "type": "label", "name": "line", "text": "ab{pause=0.5}cd", "reveal": 10 },
    "roll": { "type": "scroll", "name": "view", "size": [50, 20],
      "children": [ { "type": "box", "name": "tall", "size": [40, 100] } ] },
    "hold": { "type": "box", "name": "x", "size": [1, 1] },
    "name": { "type": "field", "name": "entry", "max_length": 6 },
    "dim": { "type": "box", "name": "shade", "size": ["fill", "fill"],
      "children": [ { "type": "panel", "name": "fill", "fill": "gold", "size": ["fill", "fill"] }, { "type": "box", "name": "mid", "size": [10, 10] } ] } } })";

UiOpen uo(i32 layer, bool input = true, bool hold = false) {
    UiOpen o;
    o.layer = layer; o.input = input; o.hold = hold;
    return o;
}

bool put(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fputs(text, f);
    return fclose(f) == 0;
}

} // namespace

void test_ui(kv::TestCtx& c) {
    using namespace kv;
    c.begin("ui");
    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &asset_module, &sprite_module, &audio_module, &ui_module, &k_t};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KV_CHECK(c, input_actions(e, k_actions));
    char root[512], path[600];
    // The fixtures go in a scratch directory mounted over the defaults, not beside the
    // executable: an iOS app's bundle is signed and read-only.
    char scratch[400];
    KV_CHECK(c, io_make_temp_dir("korvid-ui-test", scratch, sizeof scratch) && platform_io(e)->mount_dir(scratch));
    snprintf(root, sizeof root, "%s/ui_test", scratch);
    io_make_dirs(root);
    snprintf(path, sizeof path, "%s/test.ui.json", root);
    KV_CHECK(c, put(path, k_doc));
    auto frames = [&](u32 n) { for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, 1.0 / 60.0); } };

    Handle doc = ui_load(e, "ui_test/test.ui.json", Handle{});
    asset_ref(e, doc);
    for (u32 i = 0; i < 30 && !ui_ready(e, doc); ++i) frames(1);
    KV_CHECK(c, ui_ready(e, doc));

    // ---- a screen from the document; the same one from code -----------------------------
    Handle menu = ui_open(e, doc, "menu");
    frames(1);
    KV_CHECK(c, ui_find(e, menu, "") == 0 && ui_find(e, menu, "items/save") != KV_UI_NONE && ui_find(e, menu, "quit") != KV_UI_NONE);
    KV_CHECK(c, ui_find(e, menu, "items/nope") == KV_UI_NONE);
    Handle built = ui_open_empty(e, doc, "menu-code", uo(50, false));
    UiNodeDesc box;
    box.type = UiType::Panel; box.name = "box"; box.w_mode = UiSize::Fixed; box.size = vec2{100, 0};
    box.padding[0] = 4; box.padding[1] = 6; box.padding[2] = 4; box.padding[3] = 6; box.fill = "gold";
    u32 b0 = ui_add(e, built, KV_UI_NONE, box);
    UiNodeDesc items;
    items.type = UiType::List; items.name = "items";
    u32 l0 = ui_add(e, built, b0, items);
    const char* names[] = {"resume", "save", "quit"}, *texts[] = {"Resume", "Save", "Quit"};
    for (u32 i = 0; i < 3; ++i) { UiNodeDesc it; it.type = UiType::Label; it.name = names[i]; it.text = texts[i]; if (i == 2) it.event = "leave"; ui_add(e, built, l0, it); }
    UiNodeDesc corner;
    corner.name = "corner"; corner.anchor = corner.pivot = UiPoint::BottomRight; corner.w_mode = corner.h_mode = UiSize::Fixed; corner.size = vec2{10, 10}; corner.offset = vec2{-1, -2};
    ui_add(e, built, b0, corner);
    frames(1);
    const char* const paths[] = {"", "items", "items/resume", "items/save", "quit", "corner"};
    for (const char* p : paths) {
        rect2 a = ui_rect(e, menu, p), b = ui_rect(e, built, p);
        KV_CHECK(c, a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h);
    }

    // ---- layout: anchors, pivots, padding, fit, stacks ------------------------------------
    const UiStats* st = ui_stats(e);
    f32 W = 1280.0f / st->scale, H = 720.0f / st->scale;
    KV_CHECK(c, st->scale == 4.0f); // 720 / 180, whole
    rect2 r = ui_rect(e, menu, "");
    KV_CHECK(c, r.w == 100.0f && fabsf(r.x - (W - 100.0f) * 0.5f) < 0.01f && fabsf(r.y + r.h * 0.5f - H * 0.5f) < 0.01f);
    rect2 cr = ui_rect(e, menu, "corner");
    KV_CHECK(c, cr.x + cr.w == r.x + r.w - 4.0f - 1.0f && cr.y + cr.h == r.y + r.h - 6.0f - 2.0f); // bottom-right of the inside, offset
    Handle hud = ui_open(e, doc, "hud", uo(90, false));
    ui_model_set_int(e, "hp", 3);
    ui_model_set_int(e, "hp_max", 5);
    frames(1);
    rect2 bar = ui_rect(e, hud, ""), a = ui_rect(e, hud, "a"), bb = ui_rect(e, hud, "b"), hp = ui_rect(e, hud, "hp");
    KV_CHECK(c, bar.x == 0 && bar.y == 0 && a.x == 3 && a.y == 3 && bb.x == 3 + 8 + 2 && hp.x == bb.x + 12 + 2);
    KV_CHECK(c, bar.w == 3 + 8 + 2 + 12 + 2 + 30 + 3 && bar.h == 3 + 9 + 3);
    ui_close(e, hud);

    // ---- a list: focus, repeat, select by name and event, cancel ---------------------------
    g = Seen{};
    KV_CHECK(c, ui_top(e) == menu && !strcmp(ui_focused(e, menu, "items"), "resume"));
    input_action_override(e, "move", true, vec2{0, -1});
    frames(1);
    KV_CHECK(c, !strcmp(ui_focused(e, menu, "items"), "save"));
    for (u32 i = 0; i < 10; ++i) { input_action_override(e, "move", true, vec2{0, -1}); frames(1); }
    KV_CHECK(c, !strcmp(ui_focused(e, menu, "items"), "save")); // held, not yet repeating
    for (u32 i = 0; i < 8; ++i) { input_action_override(e, "move", true, vec2{0, -1}); frames(1); }
    KV_CHECK(c, !strcmp(ui_focused(e, menu, "items"), "quit")); // the repeat
    frames(1);
    input_action_override(e, "confirm", true);
    frames(2);
    KV_CHECK(c, g.events == 1 && g.last_node == name_lookup("quit").hash && g.last_event == name_lookup("leave").hash && g.last_value == 2);
    KV_CHECK(c, ui_focus(e, menu, "items", "resume"));
    input_action_override(e, "confirm", true);
    frames(2);
    KV_CHECK(c, g.last_node == name_lookup("resume").hash && g.last_event == name_lookup("select").hash);
    input_action_override(e, "cancel", true);
    frames(2);
    KV_CHECK(c, g.last_node == name_lookup("items").hash && g.last_event == name_lookup("cancel").hash);
    ui_close(e, built);

    // ---- a reveal: its rate, its pause, a confirm finishes it, then says so ---------------
    Handle talk = ui_open(e, doc, "talk", uo(200));
    frames(1);
    KV_CHECK(c, ui_top(e) == talk && ui_revealing(e, talk, "line"));
    frames(12); // 0.2 s: "ab" (0.1, 0.2), then the half-second pause
    KV_CHECK(c, ui_revealing(e, talk, "line"));
    g = Seen{};
    frames(48); // 1.0 s: c at 0.8, d at 0.9: done
    KV_CHECK(c, !ui_revealing(e, talk, "line"));
    frames(1);
    KV_CHECK(c, g.events == 1 && g.last_event == name_lookup("revealed").hash);
    ui_set_text(e, talk, "line", "again");
    frames(1);
    KV_CHECK(c, ui_revealing(e, talk, "line"));
    g = Seen{};
    input_action_override(e, "confirm", true);
    frames(2);
    KV_CHECK(c, !ui_revealing(e, talk, "line") && g.last_event == name_lookup("revealed").hash); // the confirm went to the reveal
    input_action_override(e, "confirm", true);
    frames(2);
    KV_CHECK(c, g.last_event == name_lookup("confirm").hash); // and the next one to the screen
    ui_close(e, talk);

    // ---- a scroll clamps; a holding screen holds ----------------------------------------
    Handle roll = ui_open(e, doc, "roll", uo(10, false));
    frames(1);
    ui_scroll(e, roll, "view", 500.0f);
    frames(1);
    rect2 view = ui_rect(e, roll, "view"), tall = ui_rect(e, roll, "tall");
    KV_CHECK(c, fabsf((tall.y + tall.h) - (view.y + view.h)) < 0.01f); // scrolled to the bottom, no further
    ui_scroll(e, roll, "view", -1000.0f);
    frames(1);
    KV_CHECK(c, ui_rect(e, roll, "tall").y == view.y);
    ui_close(e, roll);
    Handle dim = ui_open(e, doc, "dim", uo(5, false));
    frames(1);
    rect2 shade = ui_rect(e, dim, ""), fill = ui_rect(e, dim, "fill");
    KV_CHECK(c, shade.w == W && shade.h == H && fill.x == 0 && fill.y == 0 && fill.w == W && fill.h == H);
    ui_close(e, dim);
    Handle holder = ui_open(e, doc, "hold", uo(100, true, true));
    frames(1);
    KV_CHECK(c, (engine_holds(e) & HoldUi) != 0);
    ui_close(e, holder);
    frames(1);
    KV_CHECK(c, (engine_holds(e) & HoldUi) == 0);
    ui_close(e, menu);

    // ---- a field: typed text, erased, capped, submitted ---------------------------------------
    Handle name = ui_open(e, doc, "name", uo(300));
    frames(1);
    g = Seen{};
    input_text_override(e, "h\xc3\xa9llo");
    frames(2);
    KV_CHECK(c, !strcmp(ui_field_text(e, name, "entry"), "h\xc3\xa9llo") && g.last_event == name_lookup("changed").hash && g.last_value == 5);
    input_text_override(e, "", 2);
    frames(2);
    KV_CHECK(c, !strcmp(ui_field_text(e, name, "entry"), "h\xc3\xa9l"));
    input_action_override(e, "move", true, vec2{0, -1}); // the move action is not the field's
    input_text_override(e, "abcdef");
    frames(2);
    KV_CHECK(c, !strcmp(ui_field_text(e, name, "entry"), "h\xc3\xa9labc")); // six characters at most
    input_text_override(e, "\n");
    frames(2);
    KV_CHECK(c, g.last_event == name_lookup("submit").hash && g.last_value == 6);

    // ---- a motion out: still there until it has gone ------------------------------------------
    UiMotion out;
    out.fx = UiFx::Fade; out.seconds = 0.1f;
    KV_CHECK(c, ui_close_with(e, name, out) && ui_is_open(e, name));
    frames(8);
    KV_CHECK(c, !ui_is_open(e, name));

    // ---- a cover: closes, says so, waits while asked, opens, says so; holds meanwhile --------
    UiCover cov;
    cov.fx = UiFx::Iris; cov.seconds = 0.1f; cov.user = 77;
    g = Seen{};
    u32 id = ui_cover(e, cov);
    ui_cover_wait(e, id, true);
    frames(4);
    KV_CHECK(c, ui_covering(e) && (engine_holds(e) & HoldUi) && g.covered == 0);
    frames(6);
    KV_CHECK(c, g.covered == 1 && g.user == 77 && g.uncovered == 0);
    frames(10);
    KV_CHECK(c, ui_covering(e) && g.uncovered == 0); // waiting
    ui_cover_wait(e, id, false);
    frames(12);
    KV_CHECK(c, !ui_covering(e) && g.uncovered == 1 && (engine_holds(e) & HoldUi) == 0);

    asset_unref(e, doc);
    frames(2);
    KV_CHECK(c, !ui_ready(e, doc));
    engine_shutdown(e);
    io_remove(path);
    io_remove(root);
}
