// Behaviour and the keyboard (docs/PRDs/Krome-Slice-4-PRD.md, milestone 2): a prelude's
// script returning a behaviour, the same slider written in C++, both driving the same
// markup through the real input path (press, drag, release, keys, the wheel); the instance
// and its state; Tab order with taborder, the focused look only by keyboard, Space and
// Enter, a modal holding the keys; shortcuts; an edit's secret.
#include "korvid/krome/krome.h"
#include "../src/internal.h"
#include "korvid/asset/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/kbor.h"
#include "korvid/core/test.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/module.h"
#include <stdlib.h>
#include <string.h>

namespace {

using namespace kv;

// The prelude: its script returns the slider's behaviour.
const char* k_prelude = R"({"kim": 1, "file": "b/sl.kim", "scripts": ["b/sl.lua"]})";
const char* k_prelude_script = R"lua(
local function at(w, x)
  local k = math.max(0, math.min(1, x / w.rect.w))
  local v = w.lo + (w.hi - w.lo) * k
  if w.step > 0 then v = math.floor(v / w.step + 0.5) * w.step end
  return v
end
return {
  lslider = {
    press   = function(w, x, y, button) w:set(at(w, x)); w.state.presses = (w.state.presses or 0) + 1 end,
    drag    = function(w, x, y) w:set(at(w, x)) end,
    release = function(w, x, y) w:emit("slid", w.value) end,
    key     = function(w, name, mods)
      if name == "right" then w:set(math.min(w.hi, w.value + w.step)); return true end
      if name == "left" then w:set(math.max(w.lo, w.value - w.step)); return true end
    end,
    wheel   = function(w, dy) w:set(math.max(w.lo, math.min(w.hi, w.value + dy * w.step))); return true end,
    focus   = function(w, on) w:emit(on and "got" or "lost", w.state.presses or 0) end,
  },
}
)lua";

// The screen: the two sliders, buttons with shortcuts, a secret, tab order.
const char* k_screen = R"({"kim": 1, "file": "b/s.kim", "uses": ["b/sl"], "scripts": ["b/s.lua"],
  "screens": { "main": { "w": "box", "p": { "dir": "col", "pad": 10, "gap": 10 }, "c": [
    { "w": "box", "id": "a", "b": "a", "r": [0, 100], "p": { "behave": "lslider", "step": 5, "w": 200, "h": 20 } },
    { "w": "box", "id": "b", "b": "b", "r": [0, 100], "p": { "behave": "cslider", "step": 5, "w": 200, "h": 20 } },
    { "w": "hit", "id": "go", "e": "go", "p": { "w": 50, "h": 20, "key": "mod+s" },
      "looks": { "focused": { "bg": "#ff0000" } } },
    { "w": "hit", "id": "later", "e": "go", "f": ["disabled"], "p": { "w": 50, "h": 20, "key": "mod+d" } },
    { "w": "edit", "id": "pw", "b": "pw", "f": ["secret"], "p": { "w": 200, "h": 24 } },
    { "w": "hit", "id": "first", "e": "first", "p": { "w": 10, "h": 10, "taborder": -1 } },
    { "w": "box", "id": "plain", "p": { "w": 10, "h": 10 } },
    { "w": "hit", "id": "dialog", "e": "dialog", "p": { "w": 10, "h": 10 } }
  ] } } })";
const char* k_screen_script = R"lua(
return {
  model = { a = 0, b = 0, pw = "", gos = 0, firsts = 0, slid = 0, got = 0, lost = 0, oks = 0 },
  on = {
    go = function() model.gos = model.gos + 1 end,
    first = function() model.firsts = model.firsts + 1 end,
    slid = function(id, v) model.slid = v end,
    got = function(id, v) model.got = model.got + 1 end,
    lost = function(id, v) model.lost = model.lost + 1 end,
    dialog = function()
      open_overlay(col { id = "dlg", w = 100, h = 60,
        hit { id = "ok", event = "ok", key = "enter", w = 40, h = 20 },
        hit { id = "cancel", event = "cancel", key = "escape", w = 40, h = 20 } }, { modal = true })
    end,
    ok = function() model.oks = model.oks + 1; close_overlay() end,
    cancel = function() close_overlay() end,
  },
}
)lua";

// The same slider in C++, with a state of its own.
struct CState { u32 presses; };
f64 c_at(const KromeWidget& w, f32 x) {
    f32 k = w.rect.w > 0 ? x / w.rect.w : 0.0f;
    k = k < 0 ? 0 : k > 1 ? 1 : k;
    f64 v = w.lo + (w.hi - w.lo) * k;
    if (w.step > 0) v = (f64)(i64)(v / w.step + 0.5) * w.step;
    return v;
}
u32 g_c_focus = 0;
KromeBehaviourDesc c_slider() {
    KromeBehaviourDesc d;
    d.press = [](KromeWidget& w, f32 x, f32, u32) { w.set(c_at(w, x)); ++((CState*)w.state)->presses; };
    d.drag = [](KromeWidget& w, f32 x, f32) { w.set(c_at(w, x)); };
    d.release = [](KromeWidget& w, f32, f32) { w.emit("slid", w.number()); };
    d.key = [](KromeWidget& w, const char* name, u32) {
        if (!strcmp(name, "right")) { f64 v = w.number() + w.step; w.set(v > w.hi ? w.hi : v); return true; }
        if (!strcmp(name, "left")) { f64 v = w.number() - w.step; w.set(v < w.lo ? w.lo : v); return true; }
        return false;
    };
    d.wheel = [](KromeWidget& w, f32 dy) { f64 v = w.number() + dy * w.step; w.set(v < w.lo ? w.lo : v > w.hi ? w.hi : v); return true; };
    d.focus = [](KromeWidget&, bool on) { if (on) ++g_c_focus; };
    d.state_size = sizeof(CState);
    return d;
}

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

void pointer(Engine& e, f32 x, f32 y, u8 buttons, f32 wheel = 0.0f) { input_pointer_override(e, x, y, buttons, wheel); frames(e, 1); }

// `mod` holds the platform's shortcut key, as Krome reads `mod+` in a shortcut: Command on
// the Apple family, Ctrl elsewhere. An edit takes either for its own commands.
#if defined(__APPLE__)
constexpr Key kModKey = Key::LGui;
#else
constexpr Key kModKey = Key::LCtrl;
#endif

void key(Engine& e, Key k, bool mod = false, bool shift = false) {
    if (mod) input_key_override(e, kModKey, true);
    if (shift) input_key_override(e, Key::LShift, true);
    frames(e, 1);
    input_key_override(e, k, true);
    input_key_override(e, k, false);
    frames(e, 1);
    if (mod) input_key_override(e, kModKey, false);
    if (shift) input_key_override(e, Key::LShift, false);
    frames(e, 1);
}

bool text_is(Engine& e, Name screen, const char* k, const char* want) {
    const char* got = krome_model_text(e, screen, k);
    if (!strcmp(got, want)) return true;
    KV_ERROR("test", "model %s is '%s', wanted '%s'", k, got, want);
    return false;
}

bool focus_is(KromeModule& s, const KromeTree& t, const char* id) {
    u32 n = krome_find(t, name_intern(id));
    if (s.focus.node == n && n != KROME_NONE) return true;
    KV_ERROR("test", "the focus is on node %u, wanted '%s' (%u)", s.focus.node, id, n);
    return false;
}

} // namespace

void test_behave(kv::TestCtx& c) {
    using namespace kv;
    c.begin("krome behave");
    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KromeModule& s = *krome_state(e);
    krome_viewport(e, 800, 600, 1.0f);
    KV_CHECK(c, krome_behaviour(e, "cslider", c_slider()));
    KV_CHECK(c, !krome_behaviour(e, "cslider", c_slider())); // one name, once
    provide_json(e, "b/sl.kimb", k_prelude);
    krome_provide(e, "b/sl.lua", k_prelude_script, strlen(k_prelude_script));
    provide_json(e, "b/s.kimb", k_screen);
    krome_provide(e, "b/s.lua", k_screen_script, strlen(k_screen_script));
    Name sc = krome_open(e, "b/s.kimb", "main");
    frames(e, 4);
    KV_CHECK(c, krome_ready(e, sc));
    KV_CHECK(c, !krome_errors(e) || krome_errors(e)->count == 0);
    const KromeTree& t = *krome_tree(e, sc);
    KV_CHECK(c, krome_behaviour_known(s, name_intern("lslider")) && krome_behaviour_known(s, name_intern("cslider")));

    // The same drive on each: press at a quarter, drag to three quarters, release.
    const char* ids[] = {"a", "b"};
    for (const char* id : ids) {
        rect2 r;
        KV_CHECK(c, krome_find_rect(e, sc, id, &r));
        f32 y = r.y + r.h * 0.5f;
        pointer(e, r.x + 50, y, 0);
        pointer(e, r.x + 50, y, 1);
        KV_CHECK(c, text_is(e, sc, id, "25"));
        pointer(e, r.x + 150, y, 1);
        KV_CHECK(c, text_is(e, sc, id, "75"));
        pointer(e, r.x + 500, y, 1); // past the end, held to it
        KV_CHECK(c, text_is(e, sc, id, "100"));
        pointer(e, r.x + 150, y, 1);
        pointer(e, r.x + 150, y, 0);
        KV_CHECK(c, text_is(e, sc, id, "75") && text_is(e, sc, "slid", "75"));
        // The press gave it the focus (by the pointer: no focused look); its keys are its.
        KV_CHECK(c, focus_is(s, t, id) && !s.focus.keyboard);
        key(e, Key::Right);
        KV_CHECK(c, text_is(e, sc, id, "80"));
        key(e, Key::Left);
        key(e, Key::Left);
        KV_CHECK(c, text_is(e, sc, id, "70"));
        // The wheel over it is its, not a scroll box's.
        pointer(e, r.x + 10, y, 0, 1.0f);
        KV_CHECK(c, text_is(e, sc, id, "75"));
    }
    // Their state: kept for the node, a press each counted.
    KV_CHECK(c, g_c_focus == 1);
    KV_CHECK(c, atoi(krome_model_text(e, sc, "got")) == 1 && atoi(krome_model_text(e, sc, "lost")) == 1);
    {
        u32 presses = 0;
        for (const KromeModule::Blob& b : s.blobs) if (b.behave == name_intern("cslider")) presses = ((CState*)b.data)->presses;
        KV_CHECK(c, presses == 1);
    }

    // Tab: taborder -1 first, then document order; the disabled and the plain skipped; the
    // focused look only now that the keyboard brought it.
    pointer(e, 700, 500, 0); // a press on nothing: nothing has the focus
    pointer(e, 700, 500, 1);
    pointer(e, 700, 500, 0);
    KV_CHECK(c, s.focus.node == KROME_NONE);
    key(e, Key::Tab);
    KV_CHECK(c, focus_is(s, t, "first") && s.focus.keyboard);
    key(e, Key::Tab);
    KV_CHECK(c, focus_is(s, t, "a"));
    key(e, Key::Tab);
    key(e, Key::Tab);
    KV_CHECK(c, focus_is(s, t, "go"));
    {
        KromeScreen* scr = krome_screen(s, sc);
        UiList l;
        l.init(mem_general());
        krome_paint_screen(s, *scr, l);
        bool red = false;
        for (u32 i = 0; i < l.count(); ++i) if ((l.words[l.offsets[i]] & 3) == 0 && l.words[l.offsets[i] + 2] == 0xFF0000FFu) red = true;
        KV_CHECK(c, red);
        l.free();
    }
    // Space and Enter fire what has the focus.
    key(e, Key::Space);
    key(e, Key::Enter);
    KV_CHECK(c, text_is(e, sc, "gos", "2"));
    key(e, Key::Tab);
    KV_CHECK(c, focus_is(s, t, "pw") && krome_focus_tree(s, nullptr));
    key(e, Key::Tab, false, true); // back
    KV_CHECK(c, focus_is(s, t, "go"));

    // Shortcuts: from anywhere in the screen; a disabled node's does nothing.
    key(e, Key::S, true);
    KV_CHECK(c, text_is(e, sc, "gos", "3"));
    key(e, Key::D, true);
    KV_CHECK(c, text_is(e, sc, "gos", "3"));

    // A secret: what is typed is kept, drawn as bullets, and never copied.
    rect2 pr;
    KV_CHECK(c, krome_find_rect(e, sc, "pw", &pr));
    pointer(e, pr.x + 10, pr.y + 10, 0);
    pointer(e, pr.x + 10, pr.y + 10, 1);
    pointer(e, pr.x + 10, pr.y + 10, 0);
    input_text_override(e, "hunter2");
    frames(e, 2);
    KV_CHECK(c, text_is(e, sc, "pw", "hunter2"));
    // A shortcut's letter typed into an edit is the edit's.
    key(e, Key::S);
    KV_CHECK(c, text_is(e, sc, "gos", "3"));
    platform_clipboard_set(e, "before");
    key(e, Key::A, true);
    key(e, Key::C, true);
    {
        Array<char> clip{};
        platform_clipboard_get(e, clip);
        clip.push(0);
        KV_CHECK(c, !strcmp(clip.data, "before"));
        clip.free();
    }

    // A modal holds the focus and the keys: Tab stays in it, Enter and Escape are its.
    KV_CHECK(c, krome_press(e, sc, "dialog"));
    frames(e, 1);
    KV_CHECK(c, s.focus.node == KROME_NONE);
    key(e, Key::Tab);
    KromeScreen* scr = krome_screen(s, sc);
    KV_CHECK(c, scr->overlays.len == 1 && s.focus.overlay == scr->overlays[0]->id);
    key(e, Key::Tab);
    key(e, Key::Tab);
    KV_CHECK(c, s.focus.overlay == scr->overlays[0]->id);
    key(e, Key::S, true); // the screen's shortcut: under the modal, nothing
    KV_CHECK(c, text_is(e, sc, "gos", "3"));
    key(e, Key::Escape);
    KV_CHECK(c, scr->overlays.len == 0 && text_is(e, sc, "oks", "0"));
    KV_CHECK(c, krome_press(e, sc, "dialog"));
    frames(e, 1);
    key(e, Key::Enter);
    KV_CHECK(c, scr->overlays.len == 0 && text_is(e, sc, "oks", "1"));

    // A prelude's behaviour of a name C++ has is refused.
    const char* clash = "return { cslider = { press = function() end } }";
    KV_CHECK(c, !krome_lua_prelude(s, clash, strlen(clash), "b/clash.lua"));
    engine_shutdown(e);
}
