// What std's widgets need of the kernel (docs/PRDs/Krome-Slice-4-PRD.md, milestone 3, the
// last before it is frozen): `if key=value` (a tab's page); `{@}` and `@@` in a definition's
// body (its use's binding); a behaviour's update drawing its parts (`part=`, w:part); a
// dropdown opening its use's children (w:open_slot) whose presses come to its pick, its
// arrows moving the focus through them (w:focus_next); w:set of a table.
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

// The prelude: a meter (update draws its fill), a dropdown (open_slot, pick, keys), a set
// toggler (w:set of a table).
const char* k_prelude = R"({"kim": 1, "file": "h/w.kim", "scripts": ["h/w.lua"],
  "defs": {
    "meter": { "body": { "w": "box", "p": { "behave": "meter", "w": 100, "h": 10 }, "c": [
      { "w": "box", "p": { "part": "fill", "h": 10, "bg": "#ffffff" } },
      { "w": "text", "t": "{@}" } ] } },
    "bar": { "body": { "w": "box", "p": { "w": "@@", "h": 4 } } },
    "pick": { "body": { "w": "hit", "id": "pick", "p": { "behave": "pick", "w": 80, "h": 20 }, "c": [
      { "w": "text", "t": "{@}" } ] } },
    "option": { "params": ["label"], "body": { "w": "hit", "p": { "w": 80, "h": 20 }, "c": [ { "w": "text", "t": "{label}" } ] } }
  } })";
const char* k_prelude_script = R"lua(
return {
  meter = {
    update = function(w) w:part("fill", "w", tostring((w.value - w.lo) / (w.hi - w.lo) * 100) .. "%") end,
  },
  pick = {
    press = function(w) w:open_slot() end,
    key = function(w, name)
      if name == "down" then w:focus_next(1) return true end
      if name == "up" then w:focus_next(-1) return true end
      if name == "enter" or name == "space" then w:open_slot(); w:focus_next(1) return true end
    end,
    pick = function(w, id, value) w:set(value); w:close(); w:focus(true); return true end,
  },
  toggles = {
    press = function(w) local t = {}; for k, v in pairs(w.value) do t[k] = v end; t.b = true; w:set(t) end,
  },
}
)lua";

const char* k_screen = R"({"kim": 1, "file": "h/s.kim", "uses": ["h/w"], "scripts": ["h/s.lua"],
  "screens": { "main": { "w": "box", "p": { "dir": "col", "pad": 10, "gap": 10 }, "c": [
    { "w": "meter", "b": "level", "r": [0, 50] },
    { "w": "bar", "b": "width" },
    { "w": "pick", "b": "kind", "c": [
      { "w": "option", "t": "texture" }, { "w": "option", "t": "sound" }, { "w": "option", "t": "font" } ] },
    { "w": "hit", "id": "tog", "b": "open", "p": { "behave": "toggles", "w": 20, "h": 20 } },
    { "w": "if", "on": "tab", "ov": "rules", "c": [ { "w": "text", "t": "the rules page" } ] },
    { "w": "if", "on": "tab", "ov": "steps", "c": [ { "w": "text", "t": "the steps page" } ] }
  ] } } })";
const char* k_screen_script = R"lua(
return { model = { level = 10, width = 30, kind = "sound", tab = "rules", open = { a = true }, picks = 0 }, on = {} }
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

void key(Engine& e, Key k) {
    frames(e, 1);
    input_key_override(e, k, true);
    input_key_override(e, k, false);
    frames(e, 2);
}

u32 text_node(const KromeTree& t, const char* want) {
    for (u32 i = 0; i < t.nodes.len; ++i)
        if (t.nodes[i].type == KromeType::Text && !(t.nodes[i].flags & KROME_DEAD) && !strcmp(t.text(i), want)) return i;
    return KROME_NONE;
}

bool text_is(Engine& e, Name screen, const char* k, const char* want) {
    const char* got = krome_model_text(e, screen, k);
    if (!strcmp(got, want)) return true;
    KV_ERROR("test", "model %s is '%s', wanted '%s'", k, got, want);
    return false;
}

} // namespace

void test_hooks(kv::TestCtx& c) {
    using namespace kv;
    c.begin("krome hooks");
    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KromeModule& s = *krome_state(e);
    krome_viewport(e, 800, 600, 1.0f);
    provide_json(e, "h/w.kimb", k_prelude);
    krome_provide(e, "h/w.lua", k_prelude_script, strlen(k_prelude_script));
    provide_json(e, "h/s.kimb", k_screen);
    krome_provide(e, "h/s.lua", k_screen_script, strlen(k_screen_script));
    Name scn = krome_open(e, "h/s.kimb", "main");
    frames(e, 4);
    KV_CHECK(c, krome_ready(e, scn));
    if (const KimErrors* er = krome_errors(e)) for (u32 i = 0; i < er->count; ++i) { char ln[300]; er->format_one(i, ln, sizeof ln); KV_ERROR("test", "%s", ln); }
    KV_CHECK(c, !krome_errors(e) || krome_errors(e)->count == 0);
    KromeScreen& sc = *krome_screen(s, scn);
    const KromeTree& t = sc.tree;

    // update drew the meter's fill from its value (10 of 0..50), and again when it changes;
    // {@} in its body shows its use's binding.
    u32 meter = KROME_NONE;
    for (u32 i = 0; i < t.nodes.len; ++i) if (t.nodes[i].behave == name_intern("meter")) meter = i;
    u32 fill = meter != KROME_NONE ? krome_find_part(t, meter, name_intern("fill")) : KROME_NONE;
    KV_CHECK(c, fill != KROME_NONE && t.nodes[fill].rect.w == 20.0f);
    KV_CHECK(c, text_node(t, "10") != KROME_NONE);
    krome_set(e, scn, "level", 40.0);
    frames(e, 2);
    KV_CHECK(c, t.nodes[fill].rect.w == 80.0f && text_node(t, "40") != KROME_NONE);

    // @@: the bar's width is its use's binding.
    {
        bool found = false;
        for (u32 i = 0; i < t.nodes.len; ++i) if (t.nodes[i].h.mode == KromeSize::Fixed && t.nodes[i].h.value == 4.0f && t.nodes[i].rect.w == 30.0f) found = true;
        KV_CHECK(c, found);
    }

    // if key=value: one page at a time.
    KV_CHECK(c, text_node(t, "the rules page") != KROME_NONE && text_node(t, "the steps page") == KROME_NONE);
    krome_set(e, scn, "tab", "steps");
    frames(e, 1);
    KV_CHECK(c, text_node(t, "the rules page") == KROME_NONE && text_node(t, "the steps page") != KROME_NONE);

    // The dropdown: a press opens its use's children below it; a press on one is its pick.
    rect2 pr;
    KV_CHECK(c, krome_find_rect(e, scn, "pick", &pr));
    KV_CHECK(c, text_node(t, "sound") != KROME_NONE);
    krome_pointer(s, pr.x + 5, pr.y + 5, true, 0, false);
    krome_pointer(s, pr.x + 5, pr.y + 5, false, 0, false);
    KV_CHECK(c, sc.overlays.len == 1 && sc.overlays[0]->slot && sc.overlays[0]->owner.node != KROME_NONE);
    krome_view_frame(s);
    const KromeTree& ot = sc.overlays[0]->tree;
    u32 font = text_node(ot, "font");
    KV_CHECK(c, font != KROME_NONE && ot.nodes[ot.root].rect.y >= pr.y + pr.h && ot.nodes[ot.root].rect.w >= pr.w);
    rect2 fr = ot.nodes[font].rect;
    krome_pointer(s, fr.x + 2, fr.y + 2, true, 0, false);
    krome_pointer(s, fr.x + 2, fr.y + 2, false, 0, false);
    frames(e, 1);
    KV_CHECK(c, sc.overlays.len == 0 && text_is(e, scn, "kind", "font") && text_node(t, "font") != KROME_NONE);
    // By the keys: Enter opens it with the first option focused, Down moves, Enter picks.
    KV_CHECK(c, s.focus.node == krome_find(t, name_intern("pick")));
    key(e, Key::Enter);
    KV_CHECK(c, sc.overlays.len == 1 && s.focus.overlay == sc.overlays[0]->id);
    key(e, Key::Down);
    key(e, Key::Enter);
    KV_CHECK(c, sc.overlays.len == 0 && text_is(e, scn, "kind", "sound") && s.focus.node == krome_find(t, name_intern("pick")) && s.focus.keyboard);

    // w:set of a table: the model's set written whole.
    KV_CHECK(c, krome_find_rect(e, scn, "tog", &pr));
    krome_pointer(s, pr.x + 5, pr.y + 5, true, 0, false);
    krome_pointer(s, pr.x + 5, pr.y + 5, false, 0, false);
    char b1[32], b2[32];
    KV_CHECK(c, !strcmp(krome_model_path_text(s, sc, "open.b", b1, sizeof b1), "true") && !strcmp(krome_model_path_text(s, sc, "open.a", b2, sizeof b2), "true"));
    engine_shutdown(e);
}
